/* PipeWire SPA AEC WebRTC2 plugin with full EchoCanceller3 double-talk tuning */
/* SPDX-License-Identifier: MIT */

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

struct impl_data {
	struct spa_handle handle;
	struct spa_audio_aec aec;

	struct spa_log *log;
	rtc::scoped_refptr<webrtc::AudioProcessing> apm;
	spa_audio_info_raw rec_info;
	spa_audio_info_raw out_info;
	spa_audio_info_raw play_info;
	std::unique_ptr<float *[]> play_buffer, rec_buffer, out_buffer;
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

static void webrtc_get_spa_int(struct impl_data *impl, const struct spa_dict *args, const char *key, int *value)
{
	if (auto str = spa_dict_lookup(args, key))
		if (!spa_atoi32(str, value, 10))
			spa_log_warn(impl->log, "Could not parse '%s'", key);
}

static void webrtc_get_spa_float(struct impl_data *impl, const struct spa_dict *args, const char *key, float *value)
{
	if (auto str = spa_dict_lookup(args, key))
		if (!spa_atof(str, value))
			spa_log_warn(impl->log, "Could not parse '%s'", key);
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
	bool gain_control = webrtc_get_spa_bool(args, "webrtc.gain_control", false);
	bool gain_control1 = webrtc_get_spa_bool(args, "webrtc.gain_control1.enabled", gain_control);
	bool gain_control2 = webrtc_get_spa_bool(args, "webrtc.gain_control2.enabled", gain_control);
	bool linear_aec_ns = webrtc_get_spa_bool(args, "webrtc.linear_aec_ns", true);

	webrtc::AudioProcessing::Config config;
	config.echo_canceller.enabled = true;
	config.echo_canceller.mobile_mode = mobile_mode;
	config.echo_canceller.export_linear_aec_output = linear_aec_ns;
	config.pipeline.multi_channel_capture = rec_info->channels > 1;
	config.pipeline.multi_channel_render = play_info->channels > 1;
	config.high_pass_filter.enabled = high_pass_filter;
	config.transient_suppression.enabled = transient_suppression;

	config.noise_suppression.enabled = noise_suppression;
	config.noise_suppression.analyze_linear_aec_output_when_available = linear_aec_ns;
	config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
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

	const char *str;
	auto agc1_mode = webrtc::AudioProcessing::Config::GainController1::Mode::kAdaptiveDigital;
	str = spa_dict_lookup(args, "webrtc.gain_control1.mode");
	if (spa_streq(str, "adaptive-digital"))
		agc1_mode = webrtc::AudioProcessing::Config::GainController1::Mode::kAdaptiveDigital;
	else if (spa_streq(str, "fixed-digital"))
		agc1_mode = webrtc::AudioProcessing::Config::GainController1::Mode::kFixedDigital;

	config.gain_controller1.enabled = gain_control1;
	config.gain_controller1.mode = agc1_mode;
	config.gain_controller1.analog_gain_controller.enabled = false;
	config.gain_controller1.enable_limiter =
		webrtc_get_spa_bool(args, "webrtc.gain_control1.enable_limiter", true);
	webrtc_get_spa_int(impl, args, "webrtc.gain_control1.target_level_dbfs", &config.gain_controller1.target_level_dbfs);
	webrtc_get_spa_int(impl, args, "webrtc.gain_control1.compression_gain_db", &config.gain_controller1.compression_gain_db);

	config.gain_controller2.enabled = gain_control2;
	config.gain_controller2.adaptive_digital.enabled =
		webrtc_get_spa_bool(args, "webrtc.gain_control2.adaptive_digital.enabled", gain_control2);
	webrtc_get_spa_float(impl, args, "webrtc.gain_control2.adaptive_digital.headroom_db",
			&config.gain_controller2.adaptive_digital.headroom_db);
	webrtc_get_spa_float(impl, args, "webrtc.gain_control2.adaptive_digital.max_gain_db",
			&config.gain_controller2.adaptive_digital.max_gain_db);
	webrtc_get_spa_float(impl, args, "webrtc.gain_control2.adaptive_digital.initial_gain_db",
			&config.gain_controller2.adaptive_digital.initial_gain_db);
	webrtc_get_spa_float(impl, args, "webrtc.gain_control2.adaptive_digital.max_gain_change_db_per_second",
			&config.gain_controller2.adaptive_digital.max_gain_change_db_per_second);
	webrtc_get_spa_float(impl, args, "webrtc.gain_control2.adaptive_digital.max_output_noise_level_dbfs",
			&config.gain_controller2.adaptive_digital.max_output_noise_level_dbfs);
	webrtc_get_spa_float(impl, args, "webrtc.gain_control2.fixed_digital.gain_db",
			&config.gain_controller2.fixed_digital.gain_db);

	config.capture_level_adjustment.enabled =
		webrtc_get_spa_bool(args, "webrtc.capture_level_adjust.enabled", false);
	webrtc_get_spa_float(impl, args, "webrtc.capture_level_adjust.pre_gain_factor",
			&config.capture_level_adjustment.pre_gain_factor);
	webrtc_get_spa_float(impl, args, "webrtc.capture_level_adjust.post_gain_factor",
			&config.capture_level_adjustment.post_gain_factor);

	// Build EchoCanceller3Config with full-bandwidth double-talk preservation
	webrtc::EchoCanceller3Config aec_cfg =
		(rec_info->channels > 1 || play_info->channels > 1)
		? webrtc::EchoCanceller3::CreateDefaultMultichannelConfig()
		: webrtc::EchoCanceller3Config();
	aec_cfg.filter.export_linear_aec_output = linear_aec_ns;

	bool crisp_doubletalk = webrtc_get_spa_bool(args, "webrtc.crisp_doubletalk", true);
	if (crisp_doubletalk) {
		// Prevent >1.8kHz low-pass ("underwater") clamping when user speaks over music/podcasts
		aec_cfg.suppressor.nearend_tuning.mask_lf.enr_transparent = 4.0f;
		aec_cfg.suppressor.nearend_tuning.mask_lf.enr_suppress    = 6.0f;
		aec_cfg.suppressor.nearend_tuning.mask_hf.enr_transparent = 3.5f;
		aec_cfg.suppressor.nearend_tuning.mask_hf.enr_suppress    = 5.0f;
		aec_cfg.suppressor.nearend_tuning.max_inc_factor          = 8.0f;
		aec_cfg.suppressor.nearend_tuning.max_dec_factor_lf       = 0.5f;

		// Fast, sensitive nearend (double-talk) detection even with ambient fan noise
		aec_cfg.suppressor.dominant_nearend_detection.enr_threshold      = 1.5f;
		aec_cfg.suppressor.dominant_nearend_detection.enr_exit_threshold = 4.0f;
		aec_cfg.suppressor.dominant_nearend_detection.snr_threshold      = 6.0f;
		aec_cfg.suppressor.dominant_nearend_detection.trigger_threshold  = 3;
		aec_cfg.suppressor.dominant_nearend_detection.hold_duration      = 50;
		aec_cfg.suppressor.dominant_nearend_detection.use_unbounded_echo_spectrum = false;

		aec_cfg.suppressor.high_bands_suppression.enr_threshold        = 5.0f;
		aec_cfg.suppressor.high_bands_suppression.max_gain_during_echo = 1.0f;
		aec_cfg.suppressor.conservative_hf_suppression                 = false;
	}

	// Rawish profile: minimise non-linear suppressor pumping ("choppy"
	// with music) and let the linear adaptive filter do the cancelling.
	// Voice stays raw/transparent; residual echo during double-talk is
	// traded for natural tone. Silent-period cancellation is preserved
	// mainly by the linear filter + normal tuning below.
	bool rawish = webrtc_get_spa_bool(args, "webrtc.rawish", true);
	if (rawish) {
		// Gentle suppressor even before nearend triggers (onsets/pauses)
		aec_cfg.suppressor.normal_tuning.mask_lf.enr_transparent = 6.0f;
		aec_cfg.suppressor.normal_tuning.mask_lf.enr_suppress    = 10.0f;
		aec_cfg.suppressor.normal_tuning.mask_hf.enr_transparent = 6.0f;
		aec_cfg.suppressor.normal_tuning.mask_hf.enr_suppress    = 10.0f;
		aec_cfg.suppressor.normal_tuning.max_inc_factor          = 8.0f;
		aec_cfg.suppressor.normal_tuning.max_dec_factor_lf       = 0.5f;

		// Cap residual-echo suppression depth (ERLE) and echo estimate
		aec_cfg.erle.max_l = 2.0f;
		aec_cfg.erle.max_h = 1.2f;
		aec_cfg.ep_strength.default_gain = 0.5f;

		// Only suppress when echo is clearly audible
		aec_cfg.echo_audibility.audibility_threshold_lf = 20.0f;
		aec_cfg.echo_audibility.audibility_threshold_mf = 20.0f;
		aec_cfg.echo_audibility.audibility_threshold_hf = 20.0f;

		// Per-subband nearend exemption: keep bands containing voice
		aec_cfg.suppressor.use_subband_nearend_detection = true;
		aec_cfg.suppressor.subband_nearend_detection.subband1.low  = 1;
		aec_cfg.suppressor.subband_nearend_detection.subband1.high = 10;
		aec_cfg.suppressor.subband_nearend_detection.subband2.low  = 11;
		aec_cfg.suppressor.subband_nearend_detection.subband2.high = 22;
		aec_cfg.suppressor.subband_nearend_detection.nearend_threshold = 0.5f;
		aec_cfg.suppressor.subband_nearend_detection.snr_threshold     = 0.5f;
		spa_log_info(impl->log, "Rawish AEC3 profile enabled");
	}

	webrtc_get_spa_float(impl, args, "webrtc.nearend_hf_transparent",
			&aec_cfg.suppressor.nearend_tuning.mask_hf.enr_transparent);
	webrtc_get_spa_float(impl, args, "webrtc.nearend_hf_suppress",
			&aec_cfg.suppressor.nearend_tuning.mask_hf.enr_suppress);
	webrtc_get_spa_float(impl, args, "webrtc.nearend_lf_transparent",
			&aec_cfg.suppressor.nearend_tuning.mask_lf.enr_transparent);
	webrtc_get_spa_float(impl, args, "webrtc.nearend_lf_suppress",
			&aec_cfg.suppressor.nearend_tuning.mask_lf.enr_suppress);
	webrtc_get_spa_float(impl, args, "webrtc.nearend_snr_threshold",
			&aec_cfg.suppressor.dominant_nearend_detection.snr_threshold);
	webrtc_get_spa_int(impl, args, "webrtc.nearend_trigger_blocks",
			&aec_cfg.suppressor.dominant_nearend_detection.trigger_threshold);

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

	spa_log_info(impl->log, "WebRTC 2.1 AEC3 initialized (crisp_doubletalk=%d, linear_aec_ns=%d)",
		     crisp_doubletalk, linear_aec_ns);

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
