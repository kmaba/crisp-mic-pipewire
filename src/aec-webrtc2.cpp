/* PipeWire SPA AEC WebRTC2 plugin — Studio Crisp + Smooth Double-Talk + Strong System Audio Block */
/* SPDX-License-Identifier: MIT */

#include <cmath>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/utils/string.h>
#include <spa/utils/names.h>
#include <spa/utils/json.h>
#include <spa/support/plugin.h>

#include "api/audio/audio_processing.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_control.h"

namespace webrtc {
class EchoCanceller3 : public EchoControl {
public:
	EchoCanceller3(const EchoCanceller3Config& config,
		       const std::optional<EchoCanceller3Config>& multichannel_config,
		       int sample_rate_hz,
		       size_t num_render_channels,
		       size_t num_capture_channels);
	~EchoCanceller3() override;
	void AnalyzeRender(AudioBuffer* render) override;
	void AnalyzeCapture(AudioBuffer* capture) override;
	void ProcessCapture(AudioBuffer* capture, bool level_change) override;
	void ProcessCapture(AudioBuffer* capture, AudioBuffer* linear_output, bool level_change) override;
	Metrics GetMetrics() const override;
	void SetAudioBufferDelay(int delay_ms) override;
	void SetCaptureOutputUsage(bool capture_output_used) override;
	bool ActiveProcessing() const override;
	static EchoCanceller3Config CreateDefaultMultichannelConfig();
private:
	unsigned char storage_[0x948 - sizeof(EchoControl)];
};
} // namespace webrtc

class CustomAec3Factory : public webrtc::EchoControlFactory {
public:
	explicit CustomAec3Factory(const webrtc::EchoCanceller3Config& cfg) : cfg_(cfg) {}
	std::unique_ptr<webrtc::EchoControl> Create(int sample_rate_hz,
						    int num_render_channels,
						    int num_capture_channels) override {
		return std::unique_ptr<webrtc::EchoControl>(
			new webrtc::EchoCanceller3(cfg_, cfg_, sample_rate_hz,
						   num_render_channels, num_capture_channels));
	}
private:
	webrtc::EchoCanceller3Config cfg_;
};

// 2nd-order Direct Form II Transposed Biquad for 48kHz 32-bit float studio voicing
struct Biquad {
	float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
	float z1 = 0.0f, z2 = 0.0f;

	void set_low_shelf(float fs, float f0, float gain_db, float S = 0.85f) {
		float A = std::pow(10.0f, gain_db / 40.0f);
		float w0 = 2.0f * float(M_PI) * f0 / fs;
		float cw = std::cos(w0), sw = std::sin(w0);
		float alpha = sw / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
		float sqA2alpha = 2.0f * std::sqrt(A) * alpha;
		float a0 = (A + 1.0f) + (A - 1.0f) * cw + sqA2alpha;
		b0 = (A * ((A + 1.0f) - (A - 1.0f) * cw + sqA2alpha)) / a0;
		b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw)) / a0;
		b2 = (A * ((A + 1.0f) - (A - 1.0f) * cw - sqA2alpha)) / a0;
		a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cw)) / a0;
		a2 = ((A + 1.0f) + (A - 1.0f) * cw - sqA2alpha) / a0;
	}

	void set_high_shelf(float fs, float f0, float gain_db, float S = 0.85f) {
		float A = std::pow(10.0f, gain_db / 40.0f);
		float w0 = 2.0f * float(M_PI) * f0 / fs;
		float cw = std::cos(w0), sw = std::sin(w0);
		float alpha = sw / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
		float sqA2alpha = 2.0f * std::sqrt(A) * alpha;
		float a0 = (A + 1.0f) - (A - 1.0f) * cw + sqA2alpha;
		b0 = (A * ((A + 1.0f) + (A - 1.0f) * cw + sqA2alpha)) / a0;
		b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw)) / a0;
		b2 = (A * ((A + 1.0f) + (A - 1.0f) * cw - sqA2alpha)) / a0;
		a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cw)) / a0;
		a2 = ((A + 1.0f) - (A - 1.0f) * cw - sqA2alpha) / a0;
	}

	void set_peaking(float fs, float f0, float gain_db, float Q) {
		float A = std::pow(10.0f, gain_db / 40.0f);
		float w0 = 2.0f * float(M_PI) * f0 / fs;
		float cw = std::cos(w0), sw = std::sin(w0);
		float alpha = sw / (2.0f * Q);
		float a0 = 1.0f + alpha / A;
		b0 = (1.0f + alpha * A) / a0;
		b1 = (-2.0f * cw) / a0;
		b2 = (1.0f - alpha * A) / a0;
		a1 = (-2.0f * cw) / a0;
		a2 = (1.0f - alpha / A) / a0;
	}

	inline float process(float x) {
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return y;
	}
};

struct impl_data {
	struct spa_handle handle;
	struct spa_audio_aec aec;

	struct spa_log *log;
	rtc::scoped_refptr<webrtc::AudioProcessing> apm;
	spa_audio_info_raw rec_info;
	spa_audio_info_raw out_info;
	spa_audio_info_raw play_info;
	std::unique_ptr<float *[]> play_buffer, rec_buffer, out_buffer;
	bool studio_eq = true;
	std::vector<Biquad> eq_warmth;
	std::vector<Biquad> eq_debox;
	std::vector<Biquad> eq_air;
};

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.aec.webrtc2");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

static bool webrtc_get_spa_bool(const struct spa_dict *args, const char *key, bool default_value)
{
	if (auto str = spa_dict_lookup(args, key))
		return spa_atob(str);
	return default_value;
}

static int webrtc_init2(void *object, const struct spa_dict *args,
		struct spa_audio_info_raw *rec_info, struct spa_audio_info_raw *out_info,
		struct spa_audio_info_raw *play_info)
{
	auto impl = static_cast<struct impl_data*>(object);
	int res;

	bool high_pass_filter = webrtc_get_spa_bool(args, "webrtc.high_pass_filter", true);
	bool noise_suppression = webrtc_get_spa_bool(args, "webrtc.noise_suppression", true);
	bool transient_suppression = webrtc_get_spa_bool(args, "webrtc.transient_suppression", false);
	bool mobile_mode = webrtc_get_spa_bool(args, "webrtc.mobile_mode", false);
	bool linear_aec_ns = webrtc_get_spa_bool(args, "webrtc.linear_aec_ns", true);
	impl->studio_eq = webrtc_get_spa_bool(args, "webrtc.studio_eq", true);

	webrtc::AudioProcessing::Config config;
	config.echo_canceller.enabled = true;
	config.echo_canceller.mobile_mode = mobile_mode;
	config.echo_canceller.export_linear_aec_output = linear_aec_ns;
	config.pipeline.multi_channel_capture = rec_info->channels > 1;
	config.pipeline.multi_channel_render = play_info->channels > 1;
	config.pipeline.maximum_internal_processing_rate = 48000;
	config.high_pass_filter.enabled = high_pass_filter;
	config.high_pass_filter.apply_in_full_band = true;
	config.transient_suppression.enabled = transient_suppression;

	config.noise_suppression.enabled = noise_suppression;
	config.noise_suppression.analyze_linear_aec_output_when_available = linear_aec_ns;
	config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
	if (const char *ns_lvl = spa_dict_lookup(args, "webrtc.noise_suppression.level")) {
		if (spa_streq(ns_lvl, "low"))
			config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
		else if (spa_streq(ns_lvl, "moderate"))
			config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
		else if (spa_streq(ns_lvl, "high"))
			config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
		else if (spa_streq(ns_lvl, "very-high"))
			config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
	}

	config.gain_controller1.enabled = false;
	config.gain_controller2.enabled = false;

	// Build EchoCanceller3Config:
	// 1) Keep normal_tuning (when you are NOT speaking) at full stock strength
	//    so speaker music/podcasts are 100% blocked (-35+ dB).
	// 2) Tune nearend_tuning (when you ARE speaking) with long hold_duration
	//    and smooth gain factors so your voice is NEVER choppy and keeps full HF clarity.
	webrtc::EchoCanceller3Config aec_cfg =
		(rec_info->channels > 1 || play_info->channels > 1)
		? webrtc::EchoCanceller3::CreateDefaultMultichannelConfig()
		: webrtc::EchoCanceller3Config();
	aec_cfg.filter.export_linear_aec_output = linear_aec_ns;

	// Longer adaptive filter for tighter linear speaker subtraction in a room
	aec_cfg.filter.refined.length_blocks = 20;
	aec_cfg.filter.coarse.length_blocks  = 20;
	aec_cfg.filter.refined_initial.length_blocks = 16;
	aec_cfg.filter.coarse_initial.length_blocks  = 16;

	// Smooth, non-choppy double-talk (nearend) behavior:
	// - Balanced LF/HF transparency so voice isn't muffled over music, without letting raw music flood through
	aec_cfg.suppressor.nearend_tuning.mask_lf.enr_transparent = 2.0f;
	aec_cfg.suppressor.nearend_tuning.mask_lf.enr_suppress    = 3.0f;
	aec_cfg.suppressor.nearend_tuning.mask_hf.enr_transparent = 1.6f;
	aec_cfg.suppressor.nearend_tuning.mask_hf.enr_suppress    = 2.4f;
	// - Slow down gain drops (0.65 vs stock 0.25) so transitions between syllables never chop/pump
	aec_cfg.suppressor.nearend_tuning.max_inc_factor          = 2.5f;
	aec_cfg.suppressor.nearend_tuning.max_dec_factor_lf       = 0.65f;
	aec_cfg.suppressor.nearend_average_blocks                 = 8;

	// Hold double-talk state for ~600ms (150 blocks) across syllable gaps so music doesn't chop your words
	aec_cfg.suppressor.dominant_nearend_detection.enr_threshold      = 0.6f;
	aec_cfg.suppressor.dominant_nearend_detection.enr_exit_threshold = 5.0f;
	aec_cfg.suppressor.dominant_nearend_detection.snr_threshold      = 8.0f;
	aec_cfg.suppressor.dominant_nearend_detection.trigger_threshold  = 4;
	aec_cfg.suppressor.dominant_nearend_detection.hold_duration      = 150;
	aec_cfg.suppressor.dominant_nearend_detection.use_unbounded_echo_spectrum = false;

	aec_cfg.suppressor.high_bands_suppression.enr_threshold        = 2.5f;
	aec_cfg.suppressor.high_bands_suppression.max_gain_during_echo = 1.0f;
	aec_cfg.suppressor.conservative_hf_suppression                 = false;

	webrtc::ProcessingConfig pconfig = {{
		webrtc::StreamConfig(rec_info->rate, rec_info->channels),
		webrtc::StreamConfig(out_info->rate, out_info->channels),
		webrtc::StreamConfig(play_info->rate, play_info->channels),
		webrtc::StreamConfig(play_info->rate, play_info->channels),
	}};

	if (out_info->channels != 1 && rec_info->channels != out_info->channels) {
		spa_log_error(impl->log, "Source channels must be equal to capture channels or 1");
		return -EINVAL;
	}

	auto apm = webrtc::AudioProcessingBuilder()
		.SetConfig(config)
		.SetEchoControlFactory(std::make_unique<CustomAec3Factory>(aec_cfg))
		.Create();

	if ((res = apm->Initialize(pconfig)) != webrtc::AudioProcessing::kNoError) {
		spa_log_error(impl->log, "Error initialising webrtc2 audio processing module: %d", res);
		return -EINVAL;
	}

	// Initialize 48kHz 32-bit float studio mastering EQ per output channel:
	// 1. +2.5 dB low-shelf @ 150 Hz (restores warm studio chest body)
	// 2. -2.0 dB cut @ 480 Hz Q=1.4 (removes "tin-can / boxy" laptop resonance)
	// 3. +3.5 dB high-shelf @ 7500 Hz (restores crisp 48kHz high-bitrate condenser air)
	float fs = (float)out_info->rate;
	impl->eq_warmth.resize(out_info->channels);
	impl->eq_debox.resize(out_info->channels);
	impl->eq_air.resize(out_info->channels);
	for (uint32_t c = 0; c < out_info->channels; ++c) {
		impl->eq_warmth[c].set_low_shelf(fs, 150.0f, 2.5f, 0.8f);
		impl->eq_debox[c].set_peaking(fs, 480.0f, -2.0f, 1.4f);
		impl->eq_air[c].set_high_shelf(fs, 7500.0f, 3.5f, 0.8f);
	}

	impl->apm = std::move(apm);
	impl->rec_info = *rec_info;
	impl->out_info = *out_info;
	impl->play_info = *play_info;
	impl->play_buffer = std::make_unique<float *[]>(play_info->channels);
	impl->rec_buffer = std::make_unique<float *[]>(rec_info->channels);
	impl->out_buffer = std::make_unique<float *[]>(out_info->channels);
	return 0;
}

static int webrtc_init(void *object, const struct spa_dict *args,
		const struct spa_audio_info_raw *info)
{
	int res;
	struct spa_audio_info_raw rec_info = *info;
	struct spa_audio_info_raw out_info = *info;
	struct spa_audio_info_raw play_info = *info;
	res = webrtc_init2(object, args, &rec_info, &out_info, &play_info);
	if (rec_info.channels != out_info.channels)
		res = -EINVAL;
	return res;
}

static int webrtc_run(void *object, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	auto impl = static_cast<struct impl_data*>(object);
	int res;

	webrtc::StreamConfig play_config(impl->play_info.rate, impl->play_info.channels);
	webrtc::StreamConfig rec_config(impl->rec_info.rate, impl->rec_info.channels);
	webrtc::StreamConfig out_config(impl->out_info.rate, impl->out_info.channels);

	unsigned int num_blocks = n_samples * 1000 / impl->play_info.rate / 10;

	if (n_samples * 1000 / impl->play_info.rate % 10 != 0) {
		spa_log_error(impl->log, "Buffers must be multiples of 10ms in length "
				"(currently %u samples @%d)", n_samples, impl->play_info.rate);
		return -EINVAL;
	}

	for (size_t i = 0; i < num_blocks; i++) {
		for (size_t j = 0; j < impl->play_info.channels; j++)
			impl->play_buffer[j] = const_cast<float *>(play[j]) + play_config.num_frames() * i;
		for (size_t j = 0; j < impl->rec_info.channels; j++)
			impl->rec_buffer[j] = const_cast<float *>(rec[j]) + rec_config.num_frames() * i;
		for (size_t j = 0; j < impl->out_info.channels; j++)
			impl->out_buffer[j] = out[j] + out_config.num_frames() * i;

		if ((res = impl->apm->ProcessReverseStream(impl->play_buffer.get(),
					play_config, play_config, impl->play_buffer.get())) !=
				webrtc::AudioProcessing::kNoError) {
			spa_log_error(impl->log, "Processing reverse stream failed: %d", res);
		}

		impl->apm->set_stream_delay_ms((num_blocks - 1) * 10);

		if ((res = impl->apm->ProcessStream(impl->rec_buffer.get(),
					rec_config, out_config, impl->out_buffer.get())) !=
				webrtc::AudioProcessing::kNoError) {
			spa_log_error(impl->log, "Processing stream failed: %d", res);
		}
	}

	if (impl->studio_eq) {
		for (uint32_t c = 0; c < impl->out_info.channels; ++c) {
			float *buf = out[c];
			auto &w = impl->eq_warmth[c];
			auto &d = impl->eq_debox[c];
			auto &a = impl->eq_air[c];
			for (uint32_t s = 0; s < n_samples; ++s) {
				buf[s] = a.process(d.process(w.process(buf[s])));
			}
		}
	}

	return 0;
}

static const struct spa_audio_aec_methods impl_aec = {
	.version = SPA_VERSION_AUDIO_AEC_METHODS,
	.add_listener = NULL,
	.init = webrtc_init,
	.run = webrtc_run,
	.init2 = webrtc_init2,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	auto impl = reinterpret_cast<struct impl_data*>(handle);

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_AUDIO_AEC))
		*interface = &impl->aec;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	auto impl = reinterpret_cast<struct impl_data*>(handle);
	impl->~impl_data();
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl_data);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	auto impl = new (handle) impl_data();

	impl->handle.get_interface = impl_get_interface;
	impl->handle.clear = impl_clear;

	impl->aec.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_AUDIO_AEC,
		SPA_VERSION_AUDIO_AEC,
		&impl_aec, impl);
	impl->aec.name = "webrtc2";
	impl->aec.info = NULL;
	impl->aec.latency = "480/48000";

	impl->log = static_cast<struct spa_log *>(spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log));
	spa_log_topic_init(impl->log, &log_topic);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_AUDIO_AEC,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static const struct spa_handle_factory spa_aec_webrtc_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AEC,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

extern "C" {
SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;
}

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_aec_webrtc_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
