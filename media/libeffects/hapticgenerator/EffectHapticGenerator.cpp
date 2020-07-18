/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "EffectHG"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include "EffectHapticGenerator.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <errno.h>
#include <inttypes.h>

#include <audio_effects/effect_hapticgenerator.h>
#include <audio_utils/format.h>
#include <system/audio.h>

// This is the only symbol that needs to be exported
__attribute__ ((visibility ("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
        .tag = AUDIO_EFFECT_LIBRARY_TAG,
        .version = EFFECT_LIBRARY_API_VERSION,
        .name = "HapticGenerator Library",
        .implementor = "The Android Open Source Project",
        .create_effect = android::audio_effect::haptic_generator::HapticGeneratorLib_Create,
        .release_effect = android::audio_effect::haptic_generator::HapticGeneratorLib_Release,
        .get_descriptor = android::audio_effect::haptic_generator::HapticGeneratorLib_GetDescriptor,
};

namespace android::audio_effect::haptic_generator {

// effect_handle_t interface implementation for haptic generator effect
const struct effect_interface_s gHapticGeneratorInterface = {
        HapticGenerator_Process,
        HapticGenerator_Command,
        HapticGenerator_GetDescriptor,
        nullptr /* no process_reverse function, no reference stream needed */
};

//-----------------------------------------------------------------------------
// Effect Descriptor
//-----------------------------------------------------------------------------

// UUIDs for effect types have been generated from http://www.itu.int/ITU-T/asn1/uuid.html
// Haptic Generator
static const effect_descriptor_t gHgDescriptor = {
        FX_IID_HAPTICGENERATOR_, // type
        {0x97c4acd1, 0x8b82, 0x4f2f, 0x832e, {0xc2, 0xfe, 0x5d, 0x7a, 0x99, 0x31}}, // uuid
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_FIRST,
        0, // FIXME what value should be reported? // cpu load
        0, // FIXME what value should be reported? // memory usage
        "Haptic Generator",
        "The Android Open Source Project"
};

//-----------------------------------------------------------------------------
// Internal functions
//-----------------------------------------------------------------------------

namespace {

int HapticGenerator_Init(struct HapticGeneratorContext *context) {
    context->itfe = &gHapticGeneratorInterface;

    context->config.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    context->config.inputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    context->config.inputCfg.format = AUDIO_FORMAT_PCM_FLOAT;
    context->config.inputCfg.samplingRate = 0;
    context->config.inputCfg.bufferProvider.getBuffer = nullptr;
    context->config.inputCfg.bufferProvider.releaseBuffer = nullptr;
    context->config.inputCfg.bufferProvider.cookie = nullptr;
    context->config.inputCfg.mask = EFFECT_CONFIG_ALL;
    context->config.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_ACCUMULATE;
    context->config.outputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    context->config.outputCfg.format = AUDIO_FORMAT_PCM_FLOAT;
    context->config.outputCfg.samplingRate = 0;
    context->config.outputCfg.bufferProvider.getBuffer = nullptr;
    context->config.outputCfg.bufferProvider.releaseBuffer = nullptr;
    context->config.outputCfg.bufferProvider.cookie = nullptr;
    context->config.outputCfg.mask = EFFECT_CONFIG_ALL;

    memset(context->param.hapticChannelSource, 0, sizeof(context->param.hapticChannelSource));
    context->param.hapticChannelCount = 0;
    context->param.audioChannelCount = 0;
    context->param.maxHapticIntensity = os::HapticScale::MUTE;

    context->state = HAPTICGENERATOR_STATE_INITIALIZED;
    return 0;
}

void addBiquadFilter(
        std::vector<std::function<void(float *, const float *, size_t)>> &processingChain,
        struct HapticGeneratorProcessorsRecord &processorsRecord,
        std::shared_ptr<BiquadFilter> filter) {
    // The process chain captures the shared pointer of the filter in lambda.
    // The process record will keep a shared pointer to the filter so that it is possible to access
    // the filter outside of the process chain.
    processorsRecord.filters.push_back(filter);
    processingChain.push_back([filter](float *out, const float *in, size_t frameCount) {
            filter->process(out, in, frameCount);
    });
}

/**
 * \brief build haptic generator processing chain.
 *
 * \param processingChain
 * \param processorsRecord a structure to cache all the shared pointers for processors
 * \param sampleRate the audio sampling rate. Use a float here as it may be used to create filters
 * \param channelCount haptic channel count
 */
void HapticGenerator_buildProcessingChain(
        std::vector<std::function<void(float*, const float*, size_t)>>& processingChain,
        struct HapticGeneratorProcessorsRecord& processorsRecord,
        float sampleRate, size_t channelCount) {
    float highPassCornerFrequency = 100.0f;
    auto hpf = createHPF2(highPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, hpf);
    float lowPassCornerFrequency = 3000.0f;
    auto lpf = createLPF2(lowPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, lpf);

    auto ramp = std::make_shared<Ramp>(channelCount);
    // The process chain captures the shared pointer of the ramp in lambda. It will be the only
    // reference to the ramp.
    // The process record will keep a weak pointer to the ramp so that it is possible to access
    // the ramp outside of the process chain.
    processorsRecord.ramps.push_back(ramp);
    processingChain.push_back([ramp](float *out, const float *in, size_t frameCount) {
            ramp->process(out, in, frameCount);
    });

    highPassCornerFrequency = 60.0f;
    hpf = createHPF2(highPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, hpf);
    lowPassCornerFrequency = 700.0f;
    lpf = createLPF2(lowPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, lpf);

    lowPassCornerFrequency = 5.0f;
    float normalizationPower = -0.3f;
    // The process chain captures the shared pointer of the slow envelope in lambda. It will
    // be the only reference to the slow envelope.
    // The process record will keep a weak pointer to the slow envelope so that it is possible
    // to access the slow envelope outside of the process chain.
    auto slowEnv = std::make_shared<SlowEnvelope>(
            lowPassCornerFrequency, sampleRate, normalizationPower, channelCount);
    processorsRecord.slowEnvs.push_back(slowEnv);
    processingChain.push_back([slowEnv](float *out, const float *in, size_t frameCount) {
            slowEnv->process(out, in, frameCount);
    });

    lowPassCornerFrequency = 400.0f;
    lpf = createLPF2(lowPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, lpf);
    lowPassCornerFrequency = 500.0f;
    lpf = createLPF2(lowPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, lpf);

    auto apf = createAPF2(400.0f, 200.0f, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, apf);
    apf = createAPF2(100.0f, 50.0f, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, apf);
    float allPassCornerFrequency = 25.0f;
    apf = createAPF(allPassCornerFrequency, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, apf);

    float resonantFrequency = 150.0f;
    float bandpassQ = 1.0f;
    auto bpf = createBPF(resonantFrequency, bandpassQ, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, bpf);

    float zeroQ = 8.0f;
    float poleQ = 4.0f;
    auto bsf = createBSF(resonantFrequency, zeroQ, poleQ, sampleRate, channelCount);
    addBiquadFilter(processingChain, processorsRecord, bsf);
}

int HapticGenerator_Configure(struct HapticGeneratorContext *context, effect_config_t *config) {
    if (config->inputCfg.samplingRate != config->outputCfg.samplingRate ||
        config->inputCfg.format != config->outputCfg.format ||
        config->inputCfg.format != AUDIO_FORMAT_PCM_FLOAT ||
        config->inputCfg.channels != config->outputCfg.channels ||
        config->inputCfg.buffer.frameCount != config->outputCfg.buffer.frameCount) {
        return -EINVAL;
    }
    if (&context->config != config) {
        context->processingChain.clear();
        context->processorsRecord.filters.clear();
        context->processorsRecord.ramps.clear();
        context->processorsRecord.slowEnvs.clear();
        memcpy(&context->config, config, sizeof(effect_config_t));
        context->param.audioChannelCount = audio_channel_count_from_out_mask(
                ((audio_channel_mask_t) config->inputCfg.channels) & ~AUDIO_CHANNEL_HAPTIC_ALL);
        context->param.hapticChannelCount = audio_channel_count_from_out_mask(
                ((audio_channel_mask_t) config->outputCfg.channels) & AUDIO_CHANNEL_HAPTIC_ALL);
        ALOG_ASSERT(context->param.hapticChannelCount <= 2,
                    "haptic channel count(%zu) is too large",
                    context->param.hapticChannelCount);
        context->audioDataBytesPerFrame = audio_bytes_per_frame(
                context->param.audioChannelCount, (audio_format_t) config->inputCfg.format);
        for (size_t i = 0; i < context->param.hapticChannelCount; ++i) {
            // By default, use the first audio channel to generate haptic channels.
            context->param.hapticChannelSource[i] = 0;
        }

        HapticGenerator_buildProcessingChain(context->processingChain,
                                             context->processorsRecord,
                                             config->inputCfg.samplingRate,
                                             context->param.hapticChannelCount);
    }
    return 0;
}

int HapticGenerator_Reset(struct HapticGeneratorContext *context) {
    for (auto& filter : context->processorsRecord.filters) {
        filter->clear();
    }
    for (auto& slowEnv : context->processorsRecord.slowEnvs) {
        slowEnv->clear();
    }
    return 0;
}

int HapticGenerator_SetParameter(struct HapticGeneratorContext *context,
                                 int32_t param,
                                 uint32_t size,
                                 void *value) {
    switch (param) {
    case HG_PARAM_HAPTIC_INTENSITY: {
        if (value == nullptr || size != (uint32_t) (2 * sizeof(int))) {
            return -EINVAL;
        }
        int id = *(int *) value;
        os::HapticScale hapticIntensity = static_cast<os::HapticScale>(*((int *) value + 1));
        if (hapticIntensity == os::HapticScale::MUTE) {
            context->param.id2Intensity.erase(id);
        } else {
            context->param.id2Intensity.emplace(id, hapticIntensity);
        }
        context->param.maxHapticIntensity = hapticIntensity;
        for (const auto&[id, intensity] : context->param.id2Intensity) {
            context->param.maxHapticIntensity = std::max(
                    context->param.maxHapticIntensity, intensity);
        }
        break;
    }

    default:
        ALOGW("Unknown param: %d", param);
        return -EINVAL;
    }

    return 0;
}

/**
 * \brief run the processing chain to generate haptic data from audio data
 *
 * \param processingChain the processing chain for generating haptic data
 * \param buf1 a buffer contains raw audio data
 * \param buf2 a buffer that is large enough to keep all the data
 * \param frameCount frame count of the data
 * \return a pointer to the output buffer
 */
float* HapticGenerator_runProcessingChain(
        const std::vector<std::function<void(float*, const float*, size_t)>>& processingChain,
        float* buf1, float* buf2, size_t frameCount) {
    float *in = buf1;
    float *out = buf2;
    for (const auto processingFunc : processingChain) {
        processingFunc(out, in, frameCount);
        std::swap(in, out);
    }
    return in;
}

} // namespace (anonymous)

//-----------------------------------------------------------------------------
// Effect API Implementation
//-----------------------------------------------------------------------------

/*--- Effect Library Interface Implementation ---*/

int32_t HapticGeneratorLib_Create(const effect_uuid_t *uuid,
                                  int32_t sessionId __unused,
                                  int32_t ioId __unused,
                                  effect_handle_t *handle) {
    if (handle == nullptr || uuid == nullptr) {
        return -EINVAL;
    }

    if (memcmp(uuid, &gHgDescriptor.uuid, sizeof(*uuid)) != 0) {
        return -EINVAL;
    }

    HapticGeneratorContext *context = new HapticGeneratorContext;
    HapticGenerator_Init(context);

    *handle = (effect_handle_t) context;
    ALOGV("%s context is %p", __func__, context);
    return 0;
}

int32_t HapticGeneratorLib_Release(effect_handle_t handle) {
    HapticGeneratorContext *context = (HapticGeneratorContext *) handle;
    delete context;
    return 0;
}

int32_t HapticGeneratorLib_GetDescriptor(const effect_uuid_t *uuid,
                                         effect_descriptor_t *descriptor) {

    if (descriptor == nullptr || uuid == nullptr) {
        ALOGE("%s() called with NULL pointer", __func__);
        return -EINVAL;
    }

    if (memcmp(uuid, &gHgDescriptor.uuid, sizeof(*uuid)) == 0) {
        *descriptor = gHgDescriptor;
        return 0;
    }

    return -EINVAL;
}

/*--- Effect Control Interface Implementation ---*/

int32_t HapticGenerator_Process(effect_handle_t self,
                                audio_buffer_t *inBuffer, audio_buffer_t *outBuffer) {
    HapticGeneratorContext *context = (HapticGeneratorContext *) self;

    if (inBuffer == nullptr || inBuffer->raw == nullptr
            || outBuffer == nullptr || outBuffer->raw == nullptr) {
        return 0;
    }

    // The audio data must not be modified but just written to
    // output buffer according the access mode.
    size_t audioBytes = context->audioDataBytesPerFrame * inBuffer->frameCount;
    size_t audioSampleCount = inBuffer->frameCount * context->param.audioChannelCount;
    if (inBuffer->raw != outBuffer->raw) {
        if (context->config.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE) {
            for (size_t i = 0; i < audioSampleCount; ++i) {
                outBuffer->f32[i] += inBuffer->f32[i];
            }
        } else {
            memcpy(outBuffer->raw, inBuffer->raw, audioBytes);
        }
    }

    if (context->state != HAPTICGENERATOR_STATE_ACTIVE) {
        ALOGE("State(%d) is not HAPTICGENERATOR_STATE_ACTIVE when calling %s",
                context->state, __func__);
        return -ENODATA;
    }

    if (context->param.maxHapticIntensity == os::HapticScale::MUTE) {
        // Haptic channels are muted, not need to generate haptic data.
        return 0;
    }

    // Resize buffer if the haptic sample count is greater than buffer size.
    size_t hapticSampleCount = inBuffer->frameCount * context->param.hapticChannelCount;
    if (hapticSampleCount > context->inputBuffer.size()) {
        // The context->inputBuffer and context->outputBuffer must have the same size,
        // which must be at least the haptic sample count.
        context->inputBuffer.resize(hapticSampleCount);
        context->outputBuffer.resize(hapticSampleCount);
    }

    // Construct input buffer according to haptic channel source
    for (size_t i = 0; i < inBuffer->frameCount; ++i) {
        for (size_t j = 0; j < context->param.hapticChannelCount; ++j) {
            context->inputBuffer[i * context->param.hapticChannelCount + j] =
                    inBuffer->f32[i * context->param.audioChannelCount
                            + context->param.hapticChannelSource[j]];
        }
    }

    float* hapticOutBuffer = HapticGenerator_runProcessingChain(
            context->processingChain, context->inputBuffer.data(),
            context->outputBuffer.data(), inBuffer->frameCount);
    os::scaleHapticData(hapticOutBuffer, hapticSampleCount, context->param.maxHapticIntensity);

    // For haptic data, the haptic playback thread will copy the data from effect input buffer,
    // which contains haptic data at the end of the buffer, directly to sink buffer.
    // In that case, copy haptic data to input buffer instead of output buffer.
    // Note: this may not work with rpc/binder calls
    memcpy_by_audio_format(static_cast<char*>(inBuffer->raw) + audioBytes,
                           static_cast<audio_format_t>(context->config.outputCfg.format),
                           hapticOutBuffer,
                           AUDIO_FORMAT_PCM_FLOAT,
                           hapticSampleCount);

    return 0;
}

int32_t HapticGenerator_Command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                                void *cmdData, uint32_t *replySize, void *replyData) {
    HapticGeneratorContext *context = (HapticGeneratorContext *) self;

    if (context == nullptr || context->state == HAPTICGENERATOR_STATE_UNINITIALIZED) {
        return -EINVAL;
    }

    ALOGV("HapticGenerator_Command command %u cmdSize %u", cmdCode, cmdSize);

    switch (cmdCode) {
        case EFFECT_CMD_INIT:
            if (replyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *(int *) replyData = HapticGenerator_Init(context);
            break;

        case EFFECT_CMD_SET_CONFIG:
            if (cmdData == nullptr || cmdSize != sizeof(effect_config_t)
                || replyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *(int *) replyData = HapticGenerator_Configure(
                    context, (effect_config_t *) cmdData);
            break;

        case EFFECT_CMD_RESET:
            HapticGenerator_Reset(context);
            break;

        case EFFECT_CMD_GET_PARAM:
            ALOGV("HapticGenerator_Command EFFECT_CMD_GET_PARAM cmdData %p,"
                  "*replySize %u, replyData: %p",
                  cmdData, *replySize, replyData);
            break;

        case EFFECT_CMD_SET_PARAM: {
            ALOGV("HapticGenerator_Command EFFECT_CMD_SET_PARAM cmdSize %d cmdData %p, "
                  "*replySize %u, replyData %p", cmdSize, cmdData,
                  replySize ? *replySize : 0, replyData);
            if (cmdData == nullptr || (cmdSize < (int) (sizeof(effect_param_t) + sizeof(int32_t)))
                || replyData == nullptr || replySize == nullptr ||
                *replySize != (int) sizeof(int32_t)) {
                return -EINVAL;
            }
            effect_param_t *cmd = (effect_param_t *) cmdData;
            *(int *) replyData = HapticGenerator_SetParameter(
                    context, *(int32_t *) cmd->data, cmd->vsize, cmd->data + sizeof(int32_t));
        }
            break;

        case EFFECT_CMD_ENABLE:
            if (replyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            if (context->state != HAPTICGENERATOR_STATE_INITIALIZED) {
                return -ENOSYS;
            }
            context->state = HAPTICGENERATOR_STATE_ACTIVE;
            ALOGV("EFFECT_CMD_ENABLE() OK");
            *(int *) replyData = 0;
            break;

        case EFFECT_CMD_DISABLE:
            if (replyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            if (context->state != HAPTICGENERATOR_STATE_ACTIVE) {
                return -ENOSYS;
            }
            context->state = HAPTICGENERATOR_STATE_INITIALIZED;
            ALOGV("EFFECT_CMD_DISABLE() OK");
            *(int *) replyData = 0;
            break;

        case EFFECT_CMD_SET_VOLUME:
        case EFFECT_CMD_SET_DEVICE:
        case EFFECT_CMD_SET_AUDIO_MODE:
            break;

        default:
            ALOGW("HapticGenerator_Command invalid command %u", cmdCode);
            return -EINVAL;
    }

    return 0;
}

int32_t HapticGenerator_GetDescriptor(effect_handle_t self, effect_descriptor_t *descriptor) {
    HapticGeneratorContext *context = (HapticGeneratorContext *) self;

    if (context == nullptr ||
        context->state == HAPTICGENERATOR_STATE_UNINITIALIZED) {
        return -EINVAL;
    }

    memcpy(descriptor, &gHgDescriptor, sizeof(effect_descriptor_t));

    return 0;
}

} // namespace android::audio_effect::haptic_generator
