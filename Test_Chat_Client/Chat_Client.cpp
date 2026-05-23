//test
//C++17
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <numeric>
#include <unordered_map>
#include <functional>
#include <cstdlib>
#include <cmath> 
#include <deque>
#include <iostream>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <filesystem>
//#include <windows.h>
//#include <wincrypt.h>
//#include <cryptuiapi.h>
//#include <tchar.h>
//#pragma comment (lib, "crypt32.lib")
//#pragma comment (lib, "cryptui.lib")
#include <boost/winapi/config.hpp>//must include to set win version for atomic
#include "boost/asio.hpp"
#include "boost/thread.hpp"
#include "boost/chrono.hpp"
#include "boost/atomic.hpp"
#include "Chat_Message.h"
#include "Chat_Participant.h"
#include "Voice_Chat_Message.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_stdlib.h"
//#include "kiss_fft.h"
#include "opus.h"
#include "openssl/evp.h"
#include "openssl/bio.h"
#include "openssl/err.h"
#include "openssl/applink.c"
//must include this after boost includes to avoid winsock.h conflict
#include <windows.h>
#include <wincrypt.h>
#include <cryptuiapi.h>
#include <tchar.h>
#define MY_ENCODING_TYPE  (PKCS_7_ASN_ENCODING | X509_ASN_ENCODING)

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#define BOOST_NETWORK_ENABLE_HTTPS
#include "boost/asio/ssl.hpp"

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
using std::chrono::high_resolution_clock;
std::atomic<bool> running{ true };

typedef std::deque<chat_message> chat_message_queue;
static std::vector<std::string> msg_history;
static std::vector<chat_message> rqst_history;
static bool msg_rcvd = false;
static bool prompt_for_name = false;
static bool was_focused = false;
static bool has_name = false;
static bool change_name = false;
static bool first_enter = true;
static bool server_ping = false;
static bool awaiting_change_response = false; //account for delay between client msg and server response
static bool capture_device_change = false;
static bool playback_device_change = false;
static float dots = 0;
static float ping_timer = 0;
static float ping_interval = 3;
static float gain = 0;
const std::string key = "basic_password_authorization:D";
std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
std::chrono::duration<double> delta_time = std::chrono::duration<double>(0.0f);
const uint8_t max_participants = 16;
constexpr size_t packet_size = 512;
constexpr size_t packet_count = 64;
constexpr float sample_rate = 48000.0f;
constexpr ma_uint32 frame_size = 960;
const std::string client_version = "1.0";
const std::string ip = "159.89.49.248";
const std::string port = "5000";


const size_t max_playback_size = 3840;
const int buffer_size = 38400; //19200;
const float db_threshold = 0.5f;

ma_device_info* capture_devices;
ma_uint32 capture_count = 0;
static int selected_capture = 0;
std::vector<const char*> capture_devices_names{};
std::vector<std::string> capture_devices_names_storage{};

ma_device_info* playback_devices;
ma_uint32 playback_count = 0;
static int selected_playback = 0;
std::vector<const char*> playback_devices_names{};
std::vector<std::string> playback_devices_names_storage{};

bool mic_test = false;

const int  udp_port_number = 0;
class chat_client;
class SpectralSuppressor;
void draw_error_window(std::shared_ptr<chat_client>& c, GLFWwindow* window, ImVec2& size, ImVec2& position, std::string msg);
void draw_disconnect_window(std::shared_ptr<chat_client>& c, GLFWwindow* window, ImVec2& size, ImVec2& position);
void draw_start_connection_window(std::shared_ptr<chat_client>& c, GLFWwindow* window, ImVec2& size, ImVec2& position);
template<typename Func>
void call_imgui(Func imgui_logic, GLFWwindow* window, std::shared_ptr<chat_client>& c);
void playback_callback(ma_device* pDevice, void* pFramesOut, const void* pFramesIn, ma_uint32 frameCount);
void capture_callback(ma_device* pDevice, void* pFramesOut, const void* pFramesIn, ma_uint32 frameCount);

class HighPassFilter {
public:
	void set(float sample_rate, float cutoffHz) {
		float RC = 1.0f / (2.0f * 3.14159265f * cutoffHz);
		alpha = RC / (RC + 1.0f / sample_rate);
		prevInput = 0.0f;
		prevOutput = 0.0f;
	}
	float process(float input)
	{
		float output = alpha * (prevOutput + input - prevInput);
		prevInput = input;
		prevOutput = output;
		return output;
	}

private:
	float alpha;
	float prevInput;
	float prevOutput;
};
class BiquadFilter {
public:
	void setLowPass(float sampleRate, float cutoffFreq, float Q = 0.707f) {
		float omega = 2.0f * 3.14f * cutoffFreq / sampleRate;
		float sin_omega = std::sin(omega);
		float cos_omega = std::cos(omega);
		float alpha = sin_omega / (2.0f * Q);

		float b0 = (1.0f - cos_omega) / 2.0f;
		float b1 = 1.0f - cos_omega;
		float b2 = (1.0f - cos_omega) / 2.0f;
		float a0 = 1.0f + alpha;
		float a1 = -2.0f * cos_omega;
		float a2 = 1.0f - alpha;

		// Normalize coefficients
		this->b0 = b0 / a0;
		this->b1 = b1 / a0;
		this->b2 = b2 / a0;
		this->a1 = a1 / a0;
		this->a2 = a2 / a0;

		x1 = x2 = y1 = y2 = 0.0f;
	}

	float process(float x) {
		float y = b0 * x + b1 * x1 + b2 * x2
			- a1 * y1 - a2 * y2;

		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;

		return y;
	}

private:
	float b0, b1, b2;
	float a1, a2;
	float x1, x2;
	float y1, y2;
};
class NoiseProfile {
public:
	NoiseProfile(size_t frame_size) :
		buffer(frame_size, 0.0f), alpha(0.99f), noise_floor(100.0f) {
	}
	void smooth(size_t count) {
		/*std::vector<float> smoothed(buffer); // Copy current buffer
		for (size_t i = 1; i < count - 1; ++i) {
			buffer[i] = (smoothed[i - 1] + smoothed[i] + smoothed[i + 1]) / 3.0f;
		}
		buffer[0] = (smoothed[0] + smoothed[1]) / 2.0f;
		buffer[count - 1] = (smoothed[count - 2] + smoothed[count - 1]) / 2.0f;*/
		float avg = std::accumulate(buffer.begin(), buffer.end(), 0.0f) / buffer.size();
		noise_floor = 0.05f * avg;
		float prev = buffer[0];

		for (size_t i = 1; i < count - 1; ++i) {
			buffer[i] = (prev + buffer[i]) * 0.5f; prev = buffer[i];
		}
		for (size_t i = 1; i < count - 1; ++i) {
			buffer[i] = std::max(buffer[i], noise_floor);
		}
	}
	void update(const int16_t* samples, size_t count) {
		for (size_t i = 0; i < count; ++i) {
			float sample = static_cast<float>(samples[i]);
			float energy = sample * sample;
			buffer[i] = alpha * buffer[i] + (1.0f - alpha) * energy;
		}
		if (last_update - current_time > delta_time * 5.0f) {
			last_update = current_time;
			smooth(count);
		}
	}
	float get(size_t i) const {
		if (i >= buffer.size()) return std::sqrt(noise_floor);
		return std::sqrt(buffer[i]);
	}
private:
	std::vector<float>buffer;
	float alpha;
	float noise_floor;
	std::chrono::steady_clock::time_point last_update;
};
class sound_control {
	//MARKER2
public:
	void apply_gain(float* dst, const float* src, size_t count, float gain) {
		for (size_t i = 0; i < count; ++i) {
			float sample = src[i] * gain;
			/*if (sample > db_threshold) {
				sample = db_threshold;
			}
			if (sample < -db_threshold) {
				sample = -db_threshold;
			}*/
			dst[i] = sample;
		}
	}
	void output_volume(float* dst, const float* src, size_t count, float gain) {

	}
	void apply_gain_and_upmix(float* dst, const float* src, size_t frame_count, float gain,
		uint32_t in_channels, uint32_t out_channels) {
		//std::cout << "in_channels = " << in_channels << " out_channels = " << out_channels << "\n";
		for (size_t f = 0; f < frame_count; ++f) {
			const float* in_frame = src + f * in_channels;

			float mono_value = in_frame[0] * gain;

			float* out_frame = dst + f * out_channels;

			if (in_channels == 1) {
				for (uint32_t c = 0; c < out_channels; ++c) {
					out_frame[c] = mono_value;
				}
			}
			else {
				for (uint32_t c = 0; c < out_channels; ++c) {
					float s = in_frame[c % in_channels] * gain;
					out_frame[c] = s;
				}
			}
		}
	}
};

class rbs {
public:
	ma_rb capture_rb;
	//TODO convert playback to pcm ring buffer for mixing
	ma_pcm_rb playback_rb;
	ma_uint32 bytes_per_frame;
	ma_uint32 bytes_per_sample;
	size_t size;
	ma_format network_format;
	ma_uint32 network_channels;
	OpusDecoder* decoder;

	rbs(ma_device& capture_device, ma_device& playback_device, ma_format network_format, ma_uint32 network_channels) : size{},
	network_format{network_format}, network_channels{network_channels}, decoder{nullptr}
	{
		init_capture_rb(capture_device);
		init_playback_rb(playback_device);
		initialize_decoder();
		bytes_per_frame = ma_get_bytes_per_frame(playback_rb.format,playback_rb.channels);
		bytes_per_sample = ma_get_bytes_per_sample(playback_rb.format);
	}
	void initialize_decoder() {
		int error = 0;
		decoder = opus_decoder_create(
			48000,
			network_channels,
			&error
		);
		std::cout << "decoder create\n";
	}

	bool init_capture_rb(ma_device& capture_device) {
		ma_uint32 bpf;
		ma_uint32 subBufferSizeInFrames;
		subBufferSizeInFrames = 48000;
		bpf = ma_get_bytes_per_frame(network_format, network_channels);

		//std::cout << "capture rb size in bytes = " << subBufferSizeInFrames * bpf << "\n";
		ma_result result;
		result = ma_rb_init(subBufferSizeInFrames /** bpf*/, NULL, NULL, &capture_rb);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture ring buffer\n";
			return false;
		}
		return true;
	}
	bool init_playback_rb(ma_device& playback_device) {
		ma_uint32 bpf;
		ma_uint32 subBufferSizeInFrames;
		subBufferSizeInFrames = playback_device.playback.internalPeriodSizeInFrames * 5;
		std::cout << "subBufferSizeInFrames = " << subBufferSizeInFrames << "\n";
		bpf = ma_get_bytes_per_frame(playback_device.playback.format, playback_device.playback.channels);

		std::cout << "playback rb size in bytes = " << subBufferSizeInFrames * bpf << "\n";
		ma_result result;
		result = ma_pcm_rb_init(network_format, network_channels, subBufferSizeInFrames,
			NULL, NULL, &playback_rb);
		//result = ma_rb_init(subBufferSizeInFrames * bpf, NULL, NULL, &playback_rb);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize playback ring buffer\n";
			return false;
		}
		return true;
	}
	void uninit_capture_rb() {
		std::cout << "uninit capture rb\n";
		ma_rb_uninit(&capture_rb);
	}
	void uninit_playback_rb() {
		std::cout << "uninit playback rb\n";
		ma_pcm_rb_uninit(&playback_rb);
	}
};
class Audio_Context {
public:
	Audio_Context(size_t frame_size, float sample_rate, chat_client* c_) : speaking{ false },
		current_gain{ 1.0f }, gain{ 20.0f }, noise_profile{ frame_size },
		silence_frames{ 0 }, hangover_duration{ 10 }, fading_out{ false },
		fade_index{ 0 }, fade_duration{ 30 }, rms_smoothed{ 0.0f },
		//spectral_suppressor{ frame_size },
		encoder{ nullptr }, decoder{ nullptr },
		hp_filter{},
		lp_filter{},
		sound_controls{},
		c{c_},
		capture_temp_buffer{},
		playback_temp_buffer{},
		network_format{ma_format_f32},
		network_channels{1}
	{
		if (c == nullptr) { std::cout << "audio context c = nullptr\n"; }
		else { std::cout << "audio context not nullptr\n"; }
	}
	~Audio_Context() { std::cerr << "Audio Context destructor called\n"; }
	void init() {
		init_capture_context();
		init_playback_context();
		init_capture();
		init_playback();
		std::cout << "playback format = " << playback_device.playback.format << "\n";
		std::cout << "capture format = " << capture_device.capture.format << "\n";
		std::cout << "playback channels = " << playback_device.playback.channels << "\n";
		std::cout << "capture channels = " << capture_device.capture.channels << "\n";
		if (!playback_init || !capture_init) { return; }
		mix_buffer.resize(frame_size * playback_device.playback.channels);
		std::cout << "frame_size = " << frame_size << " channels = " << playback_device.playback.channels << "\n";
		playback_temp_buffer.resize(frame_size * playback_device.playback.channels);
		std::cout << "temp_buffer size = " << playback_temp_buffer.size() << "\n";
		capture_temp_buffer.resize(frame_size * playback_device.playback.channels);
		//bytes_per_sample_playback = ma_get_bytes_per_sample(playback_device.playback.format);
		//bytes_per_frame_playback = ma_get_bytes_per_frame(playback_device.playback.format, playback_device.playback.channels);
		bytes_per_sample_playback = ma_get_bytes_per_sample(network_format);
		bytes_per_frame_playback = ma_get_bytes_per_frame(network_format, network_channels);
	}
	void initialize_encoder() {
		int err = 0;
		encoder = opus_encoder_create(
			48000,
			network_channels,
			OPUS_APPLICATION_VOIP,
			&err
		);
		if (err != OPUS_OK) {
			std::cerr << "opus encoder creation error: " << err << "\n";
		}
		opus_encoder_ctl(encoder, OPUS_SET_BITRATE(32000));
		opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
		opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
		opus_encoder_ctl(encoder, OPUS_SET_DTX(0)); 
	}
	int start_device(ma_device& device) {
		if (ma_device_start(&device) != MA_SUCCESS) {
			std::cerr << "Failed to start audio device\n";
			return -1;
		}
	}
	int stop_device(ma_device& device) {
		if (ma_device_stop(&device) != MA_SUCCESS) {
			std::cerr << "Failed to stop audio device\n";
			return -1;
		}
	}
	int init_playback_context() {
		ma_result result;
		result = ma_context_init(NULL, 0, NULL, &playback_context);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture context\n";
			return -1;
		}
	}
	int init_playback() {
		playback_init = true;
		ma_result result;
		ma_device_config deviceConfigPlayback;
		//ma_device_info* playback_devices;
		//ma_uint32 playback_count;
		result = ma_context_get_devices(&playback_context, &playback_devices, &playback_count, NULL, NULL);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to get capture devices\n";
			return -1;
		}
		playback_devices_names.clear();
		playback_devices_names_storage.clear();
		for (ma_uint32 i = 0; i < playback_count; i++) {
			//std::cout << i << ": " << playback_devices[i].name << "\n";
			playback_devices_names_storage.emplace_back(playback_devices[i].name);
			playback_devices_names.push_back(playback_devices_names_storage[i].c_str());
		}
		if (playback_count > 0) {
			if (selected_playback >= playback_count) {
				std::cout << "selected_playback >= playback_count\n";
				return -1;
			}
			//std::cout << "selecting " << playback_devices[selected_playback].name << "\n";
		}
		deviceConfigPlayback = ma_device_config_init(ma_device_type_playback);
		deviceConfigPlayback.playback.format = network_format;
		deviceConfigPlayback.playback.pDeviceID = &playback_devices[selected_playback].id;
		deviceConfigPlayback.dataCallback = playback_callback;
		//deviceConfigPlayback.pUserData = &ma_context_;
		deviceConfigPlayback.pUserData = this;

		result = ma_device_init(NULL, &deviceConfigPlayback, &playback_device);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize playback device\n";
			return -1;
		}
	}

	void start_playback() {
		running_playback = true;
		if (start_device(playback_device) == -1) {
			return;
		}
	}
	void stop_playback() {
		running_playback = false;
		boost::system::error_code ec;
		stop_device(playback_device);
	}
	void uninit_playback() {
		std::cout << "uninit playback_device\n";
		stop_playback();
		ma_device_uninit(&playback_device);
		playback_init = false;
	}
	int init_capture_context() {
		ma_result result;
		result = ma_context_init(NULL, 0, NULL, &capture_context);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture context\n";
			return -1;
		}
	}
	int init_capture() {
		capture_init = true;
		ma_result result;
		ma_device_config deviceConfigCapture;
		result = ma_context_get_devices(&capture_context, NULL, NULL, &capture_devices, &capture_count);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to get capture device\n";
			return -1;
		}
		capture_devices_names.clear();
		capture_devices_names_storage.clear();
		for (ma_uint32 i = 0; i < capture_count; i++) {
			//std::cout << i << ": " << capture_devices[i].name << "\n";
			capture_devices_names_storage.emplace_back(capture_devices[i].name);
			capture_devices_names.push_back(capture_devices_names_storage[i].c_str());
		}
		if (capture_count > 0) {
			if (selected_capture >= capture_count) {
				std::cout << "selected_capture >= capture count\n";
				return -1;
			}
			//std::cout << "selecting " << capture_devices[selected_capture].name << "\n";
		}
		deviceConfigCapture = ma_device_config_init(ma_device_type_capture);
		deviceConfigCapture.capture.format = network_format;
		deviceConfigCapture.capture.pDeviceID = &capture_devices[selected_capture].id;
		deviceConfigCapture.dataCallback = capture_callback;
		//deviceConfigCapture.pUserData = &ma_context_;
		deviceConfigCapture.pUserData = this;

		result = ma_device_init(NULL, &deviceConfigCapture, &capture_device);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture device\n";
			return -1;
		}
	}
	void start_capture() {
		running_capture = true;
		if (start_device(capture_device) == -1) {
			return;
		}
	}
	void stop_capture() {
		running_capture = false;
		boost::system::error_code ec;
		stop_device(capture_device);
	}
	void uninit_capture() {
		std::cout << "uninit_capture()\n";
		std::cout << "ma_device_uninit\n";
		ma_device_uninit(&capture_device);
		std::cout << "ma_device_uninit end\n";

		capture_init = false;
	}
	void restart_capture() {
		bool was_running_capture_ = running_capture;
		if (running_capture) {
			stop_capture();
		}
		uninit_capture();
		init_capture();
		reinit_vc_streams();
		if (was_running_capture_) {
			start_capture();
		}
	}
	void restart_playback() {
		bool was_running_playback_ = running_playback;
		if (running_playback) {
			stop_playback();
		}
		uninit_playback();
		init_playback();
		reinit_vc_streams();
		if (was_running_playback_) {
			start_playback();
		}		
	}
	bool init_capture_rb() {
		ma_uint32 bpf;
		ma_uint32 subBufferSizeInFrames;
		subBufferSizeInFrames = capture_device.capture.internalPeriodSizeInFrames * 5;
		bpf = ma_get_bytes_per_frame(capture_device.capture.format, capture_device.capture.channels);

		//std::cout << "capture rb size in bytes = " << subBufferSizeInFrames * bpf << "\n";
		ma_result result;
		//result = ma_rb_init(subBufferSizeInFrames * bpf, NULL, NULL, &capture_ctx.ring_buffer);
		result = ma_pcm_rb_init(network_format, network_channels, subBufferSizeInFrames, NULL, NULL, &capture_ring_buffer);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture ring buffer\n";
			return false;
		}
		return true;
	}
	void reinit_vc_streams() {
		std::cout << "reinit_vc_streams\n";
		auto iter = vc_streams.begin();
		for (; iter != vc_streams.end(); ++iter) {
			iter->second.uninit_capture_rb();
			iter->second.uninit_playback_rb();
			iter->second.init_capture_rb(capture_device);
			iter->second.init_playback_rb(playback_device);
		}
		std::cout << "end reinit_vc_streams\n";
	}
	void uninit_vc_streams() {
		auto iter = vc_streams.begin();
		for (; iter != vc_streams.end(); ++iter) {
			iter->second.uninit_capture_rb();
			iter->second.uninit_playback_rb();
		}
	}
	void add_stream(chat_message& m) {
		uint8_t partner_id = 0;
		std::memcpy(&partner_id, m.body() + 1, 1);
		vc_streams.insert(std::make_pair(partner_id, rbs(capture_device, playback_device, network_format, network_channels)));
		std::cout << "add_stream for partner_id[" << static_cast<int>(partner_id) << "]\n";
	}
	void refresh_devices() {
		refresh_playback_device_list();
		refresh_capture_device_list();
	}
	void refresh_capture_device_list() {
		ma_context_get_devices(&capture_context, NULL, NULL, &capture_devices, &capture_count);
		capture_devices_names.clear();
		capture_devices_names_storage.clear();
		for (ma_uint32 i = 0; i < capture_count; i++) {
			//std::cout << i << ": " << capture_devices[i].name << "\n";
			capture_devices_names_storage.emplace_back(capture_devices[i].name);
			capture_devices_names.push_back(capture_devices_names_storage[i].c_str());
		}
	}
	void refresh_playback_device_list() {
		ma_context_get_devices(&playback_context, &playback_devices, &playback_count, NULL, NULL);
		playback_devices_names.clear();
		playback_devices_names_storage.clear();
		for (ma_uint32 i = 0; i < playback_count; i++) {
			//std::cout << i << ": " << playback_devices[i].name << "\n";
			playback_devices_names_storage.emplace_back(playback_devices[i].name);
			playback_devices_names.push_back(playback_devices_names_storage[i].c_str());
		}
	}
	bool shutting_down = false;
	bool speaking;
	bool fading_out;
	float current_gain;
	float gain;
	NoiseProfile noise_profile;
	//SpectralSuppressor spectral_suppressor;
	HighPassFilter hp_filter;
	BiquadFilter lp_filter;
	sound_control sound_controls;
	ma_uint32 silence_frames;
	ma_uint32 fade_index;
	const ma_uint32 fade_duration;
	const ma_uint32 hangover_duration; // ~200ms at 48kHz
	float rms_smoothed;
	OpusEncoder* encoder;
	OpusDecoder* decoder;
	//ma_rb ring_buffer;
	ma_pcm_rb capture_ring_buffer;
	std::unordered_map<uint8_t, rbs> vc_streams;
	float input_float[frame_size]{};
	uint8_t packet[4096]{};
	std::vector<float>mix_buffer;
	std::vector<float>playback_temp_buffer;
	std::vector<float>capture_temp_buffer;
	ma_uint32 bytes_per_sample_playback;
	ma_uint32 bytes_per_frame_playback;
	ma_uint32 bytes_per_sample_capture;
	ma_uint32 bytes_per_frame_capture;
	chat_client* c;
	ma_device playback_device;
	ma_context playback_context;
	ma_device capture_device;
	ma_context capture_context;
	ma_format network_format;
	ma_uint32 network_channels;
	std::array<uint8_t, 32> session_key;
	std::array<uint8_t, 12> iv_base;
	uint32_t send_counter = 0;

	bool capture_init = false;
	bool playback_init = false;
	bool running_capture{ false };
	bool running_playback{ false };

};

enum client_state {
	ready = 1,
	awaiting_connection = 2,
	connecting = 3,
	connected = 4,
	awaiting_authentication = 5,
	authenticated = 6,
	receiving_request = 7,
	bad_version = 8,
	no_open_room = 9
};

struct client_vc_room {
	client_vc_room() : ids{} {}
	std::unordered_set<uint8_t>ids;
};

class vc_partner {

};

class chat_client : public std::enable_shared_from_this<chat_client>
{
public:
	using client_ptr = std::shared_ptr<chat_client>;
	static client_ptr create(boost::asio::io_context& io_context,
		boost::asio::ssl::context& ssl_context,
		const tcp::resolver::results_type& endpoints, GLFWwindow* window) {
		return client_ptr(new chat_client(io_context, ssl_context, endpoints,
			window));
	}

	client_state state = client_state::awaiting_connection;
	std::vector<std::string>participant_names;
	std::vector<chat_participant>participants;//not even using this currently
	std::unordered_set<uint8_t>vc_partner_ids;//pretty sure not using this either
	std::unordered_map<uint8_t, chat_participant>participant_map;
	std::unordered_map<uint8_t, participant_client_data>participant_client_map;
	std::unordered_map<uint8_t, client_vc_room> vc_rooms;//not using this
	chat_participant me;
	std::vector<std::string> msgs;
	std::unordered_set<uint8_t> requests_;
	bool authenticated = false;
	uint8_t vc_room_id;
	std::string server_ip;
	bool voice_enabled = false;
	boost::asio::ip::udp::endpoint server_endpoint;
	//Audio_Context playback_ctx;
	//Audio_Context capture_ctx;
	Audio_Context audio_ctx;
	boost::asio::io_context& io_context_;
	std::string session_token;//16 bytes/chars
	std::shared_ptr<boost::asio::steady_timer> send_timer_;
	bool stop_playback = false;
	/*bool init_playback_rb() {
		ma_uint32 bpf;
		ma_uint32 subBufferSizeInFrames;
		subBufferSizeInFrames = playback_device.playback.internalPeriodSizeInFrames * 5;
		bpf = ma_get_bytes_per_frame(playback_device.playback.format, playback_device.playback.channels);

		//std::cout << "playback rb size in bytes = " << subBufferSizeInFrames * bpf << "\n";
		ma_result result;
		result = ma_rb_init(subBufferSizeInFrames * bpf, NULL, NULL, &playback_ctx.ring_buffer);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize playback ring buffer\n";
			return -1;
		}
		return true;
	}*/
	/*void uninit_capture_rb() {
		std::cout << "uninit capture rb\n";
		ma_rb_uninit(&capture_ctx.ring_buffer);
	}*/
	/*void uninit_playback_rb() {
		std::cout << "uninit playback rb\n";
		ma_rb_uninit(&playback_ctx.ring_buffer);
	}*/
	void check_close_socket() {
		if (!audio_ctx.running_capture && !audio_ctx.running_playback) {
			boost::system::error_code ec;
			udp_socket->close();
		}
	}
	void leave() {
		chat_message m;
		m.set_message_type(message_type::leave);
		std::memcpy(m.body(), &me.id, 1);
		m.body_length(1);
		m.encode_header();
		write_ssl(m);
	}

	void try_reconnect() {
		if(!socket_->is_open()){
			socket_ = std::make_shared<tcp::socket>(io_context_);
			socket_->open(boost::asio::ip::tcp::v4()); // reuse same io_ctx			
		}
		state = client_state::connecting;
		do_connect(endpoints);
	}
	void try_reconnect_ssl() {
		/*std::cout << "try_reconnect_ssl()\n";
		ssl_socket_ = std::make_unique<boost::asio::ssl::stream<tcp::socket>>(io_context_, ssl_context_);
		ssl_socket_->set_verify_mode(boost::asio::ssl::verify_peer);
		ssl_socket_->set_verify_callback(
			std::bind(&chat_client::verify_certificate, this, std::placeholders::_1, std::placeholders::_2));		
		state = client_state::connecting;
		do_connect_ssl(endpoints);*/
		do_reconnect();
	}
	void send_ready_notification() {
		std::cout << "send_ready_notification\n";
		chat_message msg;
		std::string text = "ready";
		msg.body_length(text.length());
		msg.set_message_type(message_type::ready_notification);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		//write(msg);
		write_ssl(msg);
	}
	void send_version() {
		chat_message version;
		version.body_length(client_version.length());
		version.set_message_type(message_type::version_check);
		std::memcpy(version.body(), client_version.c_str(), client_version.length());
		version.encode_header();
		std::cout << "version length = " << version.body_length() << "\n";
		write_ssl(version);
	}

	void write_ssl(const chat_message& msg) {
		boost::asio::post(io_context_,
			[this, msg]()
			{
				bool write_in_progress = !write_msgs_.empty();
				write_msgs_.push_back(msg);
				if (write_msgs_.front().msg_type == message_type::name_change_request) {
					//TODO
					//return;
				}
				if (!write_in_progress) {
					do_write_ssl();
				}
			});
	}
	void write(const chat_message& msg) {
		boost::asio::post(io_context_,
			[this, msg]()
			{
				bool write_in_progress = !write_msgs_.empty();
				write_msgs_.push_back(msg);
				if (write_msgs_.front().msg_type == message_type::name_change_request) {
					//TODO
					//return;
				}
				if (!write_in_progress) {
					do_write();
				}
			});
	}
	void close() {
		boost::asio::post(io_context_, [this]() {socket_->close(); });
		//boost::asio::post(io_context_, [this]() {ssl_socket_->async_shutdown(); });
		//boost::asio::post(io_context_, [this]() {udp_socket->close(); });
	}
	void schedule_check_and_send_test() {
		auto self = shared_from_this();
		boost::asio::post(io_context_, [self] {
			self->send_timer_->expires_after(boost::asio::chrono::milliseconds(10));
			auto timer_cpy = self->send_timer_;
			self->send_timer_->async_wait([self, timer_cpy](const boost::system::error_code& ec) {
				self->check_and_send_test();
				});
			}
		);

	}
	void check_and_send_test() {
		if (!audio_ctx.running_capture) { std::cout << "NOT RUNNING CAPTURE!\n"; return; }
		ma_result result;
		ma_uint32 frames_to_read = frame_size;
		void* pOut = nullptr;
		result = ma_pcm_rb_acquire_read(&audio_ctx.capture_ring_buffer, &frames_to_read, &pOut);
		if (result == MA_SUCCESS && /*size*/ frames_to_read > 0) {
			ma_uint32 bytes_per_frame = ma_get_bytes_per_frame(
				audio_ctx.network_format,
				audio_ctx.network_channels);
			size_t pcm_bytes = frames_to_read * bytes_per_frame;
			if (pcm_bytes > max_playback_size) {
				pcm_bytes = max_playback_size;
				frames_to_read = (ma_uint32)(pcm_bytes / bytes_per_frame);
				pcm_bytes = frames_to_read * bytes_per_frame;
			}
			const float* pcm = (const float*)pOut;
			unsigned char opus_packet[4000];
			int encoded_bytes = opus_encode_float(
				audio_ctx.encoder,
				pcm,
				frames_to_read,
				opus_packet,
				sizeof(opus_packet)
			);
			if (encoded_bytes <= 0) {
				ma_pcm_rb_commit_read(&audio_ctx.capture_ring_buffer, 0);
				return;
			}
			
			uint8_t nonce[12];
			build_nonce(audio_ctx.iv_base, audio_ctx.send_counter, nonce);

			uint8_t ciphertext[4000];
			uint8_t tag[16];

			std::shared_ptr<voice_chat_message> m = std::make_shared<voice_chat_message>();
			uint16_t body_len = (uint16_t)(4 + encoded_bytes + 16);
			//std::cout << "encrypt me.id = " << static_cast<int>(me.id) << "\n";
			m->encode_header(session_token, body_len, vc_room_id, me.id);
			const uint8_t* aad = (const uint8_t*)m->data();
			int aad_len = m->header_length;
			std::string header = std::string((char*)aad, aad_len);
			//std::cout << "pre-encrypt header: " << header << "\n";		
			//std::cout << "client body_len = " << body_len << "\n";
			//std::cout << "header content\n"
				//<< "\tsession_token = " << session_token
				//<< "\tbody_len = " << m->body_length()
				//<< "\tvc_room_id = " << static_cast<int>(vc_room_id)
				//<< "\tsender_id = " << static_cast<int>(me.id) << "\n";
			if (!aes_gcm_encrypt(
				audio_ctx.session_key.data(),
				nonce,
				opus_packet,
				encoded_bytes,
				aad,
				aad_len,
				ciphertext,
				tag
			)) {
				//std::cout << "fail encrypt\n";
				ma_pcm_rb_commit_read(&audio_ctx.capture_ring_buffer, 0);
				return;
			}
			//uint16_t body_len = (uint16_t)(4 + encoded_bytes + 16);
			//m->encode_header(session_token, body_len, vc_room_id, me.id);
			uint8_t* body = (uint8_t*)m->body();
			std::memcpy(body, &audio_ctx.send_counter, 4);
			std::memcpy(body + 4, ciphertext, encoded_bytes);
			std::memcpy(body + 4 + encoded_bytes, tag, 16);

			audio_ctx.send_counter++;
			/*std::shared_ptr<voice_chat_message> m = std::make_shared<voice_chat_message>();
			m->encode_header(session_token, (uint16_t)encoded_bytes, vc_room_id, me.id);
			std::memcpy(m->body(), opus_packet, encoded_bytes);*/
			auto buffer = boost::asio::buffer(m->data(), m->length());
			auto self = shared_from_this();
			udp_socket->async_send_to(buffer, server_endpoint,
				[this, self, /*size*/frames_to_read](boost::system::error_code ec, std::size_t bytes_sent/*std::size_t bytes*/) {
					//std::cout << "async_send_to lambda start\n";
					if (!ec) {
						ma_pcm_rb_commit_read(&self->audio_ctx.capture_ring_buffer, frames_to_read);
						//std::cout << "async_send_to commit_read " << frames_to_read << "\n";
					}
					else {
						std::cout << "error seding data to server\n";
						//std::cout << "error sending capture_ctx.ring_buffer data of size " << size << " error[" << ec << "]\n";
						udp_socket->close();
					}
					//boost::asio::post(io_context_, [this, self] {check_and_send();});
					//std::cout << "async_send_to lambda end\n";
				}
			);
			//std::cout << "check_and_send_test end\n";
			return;
		}
		//std::cout << "schedule retry timer\n";
		/*if (retry_read_capture_timer == nullptr) {
			auto self = shared_from_this();
			retry_read_capture_timer = std::make_shared<boost::asio::steady_timer>(io_context_);
			retry_read_capture_timer->expires_after(std::chrono::milliseconds(2));
			//	std::cout << "timer scheduled\n";
			retry_read_capture_timer->async_wait([this, self](boost::system::error_code) {
				//std::cout << "TIMER EXPIRED*****************%%%%\n";
				retry_read_capture_timer = nullptr;
				self->check_and_send_test();
				}
			);
		}*/
	}
	void check_and_read_header_test() {
		std::cout << "i'm alive\n";
		auto self = shared_from_this();
		auto read_vc_msg_ = std::make_shared<voice_chat_message>();
		udp_socket->async_receive_from(boost::asio::buffer(read_vc_msg_->data(), voice_chat_message::header_length + voice_chat_message::max_body_length), server_endpoint,
			[this, self, read_vc_msg_](boost::system::error_code ec, std::size_t bytes) {
				//std::cout << "async_receive_from lambda body start\n";
				if (!self->audio_ctx.running_playback) {
					std::cout << "!running_playback()\n";
					boost::asio::post(io_context_, [self] {
						self->check_and_read_header_test();
						});								return;
				}
				if (!ec) {
					if (!read_vc_msg_->decode_header()) {
						//std::cout << "udp_socket->async_receive_from decode header fail\n";
						boost::asio::post(io_context_, [self] {
							self->check_and_read_header_test();
							});						
						return;
					}
					//std::cout << "udp_socket->async_receive_from\n";
					check_and_read_body_test(read_vc_msg_);
					boost::asio::post(io_context_, [self] {
						self->check_and_read_header_test();
						});
				}
				else {
					std::cout << "error: " << ec.message() << " close udp socket\n";
					udp_socket->close();
				}
			});

	}

private:
	void check_and_read_body_test(std::shared_ptr<voice_chat_message> read_vc_msg_) {
		//std::cout << "check_and_read_body_test\n";
		size_t body_len = read_vc_msg_->body_length();
		uint8_t* body = (uint8_t*)read_vc_msg_->body();
		uint8_t sender_id = read_vc_msg_->sender_id;
		//std::cout << "VC PACKET from " << int(sender_id)
			//<< " body_len=" << body_len << "\n";

		if (body_len < 4 + 16) {
			//std::cout << "bad packet\n";
			return;
		}
		
		uint32_t counter;
		std::memcpy(&counter, body, 4);

		uint8_t* ciphertext = body + 4;
		size_t ct_len = body_len - 4 - 16;
		uint8_t* tag = body + 4 + ct_len;

		uint8_t nonce[12];
		build_nonce(audio_ctx.iv_base, counter, nonce);

		const uint8_t* aad = (const uint8_t*)read_vc_msg_->data();
		int aad_len = read_vc_msg_->header_length;
		std::string header = std::string((char*)aad, aad_len);
		uint8_t opus_packet[4000];

		if (!aes_gcm_decrypt(
			audio_ctx.session_key.data(),
			nonce,
			ciphertext,
			(int)ct_len,
			aad,
			aad_len,
			tag,
			opus_packet
			))
		{
			std::cout << "fail decrypt\n";
			return;
		}
		rbs& stream = audio_ctx.vc_streams.at(sender_id);
		float pcm_out[5760];
		int decoded_frames = opus_decode_float(
			stream.decoder,
			opus_packet,
			(opus_int32)ct_len,
			pcm_out,
			5760,
			0
		);
		if (decoded_frames <= 0) {
			std::cout << "0 decoded frames\n";
			return;
		}
		ma_uint32 frames_to_write = (ma_uint32)decoded_frames;
		ma_uint32 frames_written = frames_to_write;
		void* pOut = nullptr;
		ma_result result = ma_pcm_rb_acquire_write(
			&stream.playback_rb,
			&frames_written,
			&pOut
		);
		if (result != MA_SUCCESS || frames_written == 0) {
			std::cout << "result: " << result << "\n";
			return;
		}
		if (frames_written < frames_to_write) {
			//std::cout << "PLAYBACK_RB WRITE UNDERRUN: wanted "
			//	<< frames_to_write << " wrote " << frames_written << "\n";
			return;
		}

		ma_uint32 bpf = audio_ctx.bytes_per_frame_playback;
		size_t bytes_to_write = frames_written * bpf;
		std::memcpy(pOut, pcm_out, bytes_to_write);
		ma_pcm_rb_commit_write(&stream.playback_rb, frames_written);
		//std::cout << "write to playback " << frames_written << "\n";
		/*ma_result result;
		size_t encoded_size = read_vc_msg_->body_length();
		uint8_t* data = (uint8_t*)read_vc_msg_->body();
		uint8_t sender_id = read_vc_msg_->sender_id;

		rbs& stream = audio_ctx.vc_streams.at(sender_id);

		const unsigned char* opus_in = (const unsigned char*)data;

		float pcm_out[5760 * 2];
		int decoded_frames = opus_decode_float(
			stream.decoder,
			opus_in,
			(opus_int32)encoded_size,
			pcm_out,
			5760,
			0
		);

		if (decoded_frames <= 0) {
			return;
		}

		ma_uint32 frames_to_write = (ma_uint32)decoded_frames;
		ma_uint32 frames_written = frames_to_write;
		void* pOut = nullptr;
		result = ma_pcm_rb_acquire_write(
			&stream.playback_rb,
			&frames_written,
			&pOut
		);
		if (result != MA_SUCCESS || frames_written == 0) {
			return;
		}
		ma_uint32 bpf = audio_ctx.bytes_per_frame_playback;
		size_t bytes_to_write = frames_written * bpf;
		std::memcpy(pOut, pcm_out, bytes_to_write);
		ma_pcm_rb_commit_write(&stream.playback_rb, frames_written);*/
	}
	std::shared_ptr<boost::asio::steady_timer> retry_read_capture_timer;
	void add_participants(chat_message& m) {
		auto iter = participant_client_map.find(me.id);
		std::pair<bool, bool> enable_vc{};
		if (iter != participant_client_map.end()) {
			enable_vc = iter->second.enable_vc;
		}
		participants.clear();
		participant_map.clear();
		participant_client_map.clear();
		if (m.body_length() < sizeof(uint8_t)) { return; }
		uint8_t ps = 0;
		char* ptr = m.body();
		std::memcpy(&ps, ptr, sizeof(uint8_t));
		//std::cout << "ps[" << static_cast<int>(ps) << "]\n";
		ptr += sizeof(uint8_t);
		char* end = m.body() + m.body_length();
		//std::string mbody = std::string(m.body(), m.body_length());
		//std::cout << "\tm.body[" << mbody << "]\n";
		std::unordered_map<uint8_t, chat_participant> temp_participant_map{};
		std::unordered_map<uint8_t, participant_client_data> temp_client_data_map{};
		for (int i = 0; i < ps; i++) {
			if (ptr >= end) { break; }
			chat_participant p{};
			uint8_t len = 0;
			std::memcpy(&len, ptr, sizeof(uint8_t));
			//std::cout << "\tlen[" << static_cast<int>(len) << "]\n";
			ptr += sizeof(uint8_t);
			size_t size = p.deserialize(ptr);
			//std::cout << "\tsize[" << size << "]\n";
			ptr += len;
			//std::cout << "adding participant p:\n"
			//	<< "\tid[" << static_cast<int>(p.id) << "]"
			//	<< "\tname[" << p.name << "]\n";
			participant_client_data pcd{ p };
			if (p.id == me.id) { pcd.enable_vc = enable_vc; }
			participants.push_back(p);
			participant_map.emplace(p.id, p);
			participant_client_map.emplace(p.id,pcd);
		}
	}
	/*void send_version() {
		chat_message version;
		version.body_length(client_version.length());
		version.set_message_type(message_type::version_check);
		std::memcpy(version.body(), client_version.c_str(), client_version.length());
		version.encode_header();
		std::cout << "version length = " << version.body_length() << "\n";
		write_ssl(version);
	}*/
	void send_authentication() {
		chat_message auth;		
		auth.body_length(key.length());
		auth.set_message_type(message_type::authentication_response);
		std::memcpy(auth.body(), key.c_str(), key.length());
		auth.encode_header();
		std::string auth_string = std::string(auth.body(), auth.body_length());
		//write(auth);
		write_ssl(auth);
	}
	std::string decode_session_token(chat_message& m) {
		if (m.body_length() < sizeof(uint8_t)) { return "bad"; }
		std::string token = std::string(m.body(), m.body_length());	
		return token;
	}
	void send_start_room_request() {
		std::cout << "start room request!!!!!\n";
		chat_message msg;
		std::string text = "enter room";
		msg.body_length(text.length());
		msg.set_message_type(message_type::start_room_request);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		//write(msg);
		write_ssl(msg);
	}
	void read_id(chat_message& m) {
		std::memcpy(&me.id, m.body(), sizeof(uint8_t));
		std::cout << "receive read_id id is " << static_cast<int>(me.id) << "\n";
	}
	void store_server_port(chat_message& m) {
		/*uint8_t sender_id = 0;
		vc_partner_ids.clear(); //only will get 2 vc participants currently
		std::memcpy(&sender_id, m.body(), sizeof(sender_id));
		if (sender_id != me.id) { return; }
		me.vc_state = voice_chat_state::in_session;
		participant_client_map.at(sender_id).ps = participant_state::in_vc;
		vc_room_id = 0;
		std::memcpy(&vc_room_id, m.body() + sizeof(sender_id), sizeof(vc_room_id));
		udp_port_server = 0;
		std::memcpy(&udp_port_server, m.body() + sizeof(sender_id) + sizeof(vc_room_id), sizeof(udp_port_server));
		server_endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4(server_ip), udp_port_server);
		if (!udp_socket->is_open()) {
			udp_socket = std::make_shared<udp::socket>(io_context_, udp::endpoint(udp::v4(), 13));
			auto client_ep = udp_socket->local_endpoint();
			udp_port_client = client_ep.port();
		}*/
		uint8_t client_id = 0;
		uint8_t partner_id = 0;
		udp_port_server = 0;
		std::memcpy(&client_id, m.body(), 1);
			std::cout << "client_id[" << static_cast<int>(client_id) << "]"
				<< "\nme.id[" << static_cast<int>(me.id) << "]\n";
		std::memcpy(&partner_id, m.body() + 1, 1);
		std::cout << "partner_id[" << static_cast<int>(partner_id) << "]";
		std::memcpy(&udp_port_server, m.body() + 2, 2);
		
		std::cout << "udp_port_server[" << static_cast<int>(udp_port_server) << "]\n";
		server_endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4(server_ip), udp_port_server);
		if (!udp_socket->is_open()) {
			udp_socket = std::make_shared<udp::socket>(io_context_, udp::endpoint(udp::v4(), udp_port_number));
			auto client_ep = udp_socket->local_endpoint();
			udp_port_client = client_ep.port();
		}

	}
	void send_udp_port() {
		chat_message msg;
		std::string port = std::to_string(udp_port_client);
		msg.body_length(port.size());
		msg.set_message_type(message_type::send_udp_port);
		std::memcpy(msg.body(), port.data(), msg.body_length());
		msg.encode_header();
		//write(msg);
		write_ssl(msg);
	}
	void receive_vc_request(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), sizeof(uint8_t));
		auto it = participant_client_map.find(sender_id);
		if (it != participant_client_map.end()) {
			it->second.ps = participant_state::requesting_vc;
		}
		else {
			std::cout << "vc request from unknown user\n";
		}
		requests_.emplace(sender_id);
		state = client_state::receiving_request;
	}
	void send_vc_status(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), 1);
		auto it = participant_client_map.find(sender_id);
		if (it == participant_client_map.end()) {
			std::cout << "vc request from unknown user\n";
			return;
		}
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + 1, 1);
		if (receiver_id != me.id) {
			std::cout << "msg delivered to wrong recipient\n";
			return;
		}
		uint8_t sender_enable_vc = 0;
		std::memcpy(&sender_enable_vc, m.body() + 2, 1);
		//TODO start from here and finish status check response

		uint8_t receiver_enable_vc = participant_client_map.at(sender_id).enable_vc.first;

		chat_message msg;
		msg.body_length(sizeof(sender_id) + sizeof(receiver_id) + sizeof(sender_enable_vc) + sizeof(receiver_enable_vc) + sizeof(sender_enable_vc));
		msg.set_message_type(message_type::vc_status_response);
		std::memcpy(msg.body(), &sender_id, 1);
		std::memcpy(msg.body() + 1, &receiver_id, 1);
		std::memcpy(msg.body() + 2, &sender_enable_vc, 1);
		std::memcpy(msg.body() + 3, &receiver_enable_vc, 1);

		msg.encode_header();
		write_ssl(msg);
	}
	void vc_request_rejected(chat_message& m) {
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + sizeof(uint8_t), sizeof(uint8_t));
		auto it = participant_client_map.find(receiver_id);
		if (it != participant_client_map.end()) {
			it->second.ps = participant_state::neutral;
		}
	}
	void vc_request_accepted(chat_message& m) {
		std::cout << "vc_request_accepted()\n";
		uint8_t count = 0;
		//moving to room model and getting away from this
		//vc_partner_ids.clear(); //only will get 2 vc participants currently
		std::memcpy(&count, m.body(), sizeof(count));
		std::cout << "count[" << static_cast<int>(count) << "]\n";
		if (count > max_participants) { return; }
		vc_room_id = 0;
		std::memcpy(&vc_room_id, m.body() + sizeof(uint8_t) + count, sizeof(vc_room_id));
		std::cout << "vc_room_id[" << static_cast<int>(vc_room_id) << "]\n";
		client_vc_room cvr{};
		auto it = vc_rooms.find(vc_room_id);
		if (it == vc_rooms.end()) {
			vc_rooms.insert(std::make_pair(vc_room_id, cvr));
		}
		me.vc_state = voice_chat_state::in_session;
		for (uint8_t i = 0; i < count; i++) {
			uint8_t temp_id = 0;
			std::memcpy(&temp_id, m.body() + i, 1);
			std::cout << "temp_id[" << static_cast<int>(temp_id) << "]\n";
			if (temp_id != me.id) {
				if (participant_map.find(temp_id) == participant_map.end()) { continue; }
				if (vc_partner_ids.find(temp_id) != vc_partner_ids.end()) { continue; }
				participant_client_map.at(temp_id).ps = participant_state::in_vc;//will throw error if not in map
				vc_partner_ids.emplace(temp_id);
				participant_client_map.at(temp_id).ps = participant_state::in_vc;
			}
		}
		udp_port_server = 0;
		std::memcpy(&udp_port_server, m.body() + sizeof(uint8_t) + count + sizeof(vc_room_id), sizeof(udp_port_server));
		std::cout << "udp_port_server[" << static_cast<int>(udp_port_server) << "]\n";
		server_endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4(server_ip), udp_port_server);
		send_udp_port();
	}
	void start_mic_test(chat_message& m) {
		uint8_t sender_id = 0;
		vc_partner_ids.clear(); //only will get 2 vc participants currently
		std::memcpy(&sender_id, m.body(), sizeof(sender_id));
		if (sender_id != me.id) { return; }
		me.vc_state = voice_chat_state::in_session;
		participant_client_map.at(sender_id).ps = participant_state::in_vc;
		vc_room_id = 0;
		std::memcpy(&vc_room_id, m.body() + sizeof(sender_id), sizeof(vc_room_id));
		udp_port_server = 0;
		std::memcpy(&udp_port_server, m.body() + sizeof(sender_id) + sizeof(vc_room_id), sizeof(udp_port_server));
		server_endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::make_address_v4(server_ip), udp_port_server);
		if (!udp_socket->is_open()) {
			udp_socket = std::make_shared<udp::socket>(io_context_, udp::endpoint(udp::v4(), udp_port_number));
			auto client_ep = udp_socket->local_endpoint();
			udp_port_client = client_ep.port();
		}
		send_udp_port();
	}
	/*void end_vc_old(chat_message& m) {
		uint8_t sender_id;
		std::memcpy(&sender_id, m.body(), sizeof(sender_id));
		if (sender_id != me.id) {
			return;
		}
		uint8_t count = 0;
		vc_partner_ids.clear(); //only will get 2 vc participants currently
		std::memcpy(&count, m.body() + sizeof(sender_id), sizeof(count));
		if (count > max_participants) { return; }
		for (int i = 0; i < count; i++) {
			uint8_t temp_id = 0;
			std::memcpy(&temp_id, m.body() + sizeof(sender_id) + sizeof(count) + sizeof(uint8_t) * i, sizeof(uint8_t));
			if (temp_id == me.id) {
				mic_test = false;
				me.vc_state = voice_chat_state::none;
			}
			if (participant_map.find(temp_id) == participant_map.end()) { continue; }
			participant_client_map.at(temp_id).ps = participant_state::neutral;
			participant_client_map.at(temp_id).p.vc_state = voice_chat_state::none;//p needs to be a ptr TODO
			vc_partner_ids.erase(temp_id);
		}
		vc_room_id = 0;
		stop_playback();
		//TODO revisit this
		//uninit_playback();
		//uninit_playback_rb();
		stop_capture();
		//uninit_capture();
		//uninit_capture_rb();
	}*/
	//not using this
void remove_sender_from_chat(chat_message& m) {
	uint8_t sender_id;
	std::memcpy(&sender_id, m.body(), sizeof(sender_id));
	std::cout << "remove[" << static_cast<int>(sender_id) << "] from chat\n";
	if (participant_map.find(sender_id) != participant_map.end())
	{
		std::cout << "erase [" << static_cast<int>(sender_id) << "] from participant_map\n";
		participant_map.erase(sender_id);
	}
	if (vc_partner_ids.find(sender_id) != vc_partner_ids.end()) {
		vc_partner_ids.erase(sender_id);
	}
	if (participant_client_map.find(sender_id) != participant_client_map.end()) {
		participant_client_map.erase(sender_id);
		std::cout << "erase [" << static_cast<int>(sender_id) << "] from participant_client_map\n";
	}
}
//not using this
void update_vc_partner_ids(chat_message& m) {
	uint8_t enable_vc_option = 0;
	std::memcpy(&enable_vc_option, m.body(), 1);
	uint8_t receiver_id = 0;
	std::memcpy(&receiver_id, m.body() + 1, 1);
	auto iter = vc_partner_ids.find(receiver_id);
	if (iter != vc_partner_ids.end() && enable_vc_option == 1) {
		std::cout << "vc partner already added\n";
		return;
	}
	if (iter == vc_partner_ids.end() && enable_vc_option == 1) {
		vc_partner_ids.insert(receiver_id);
	}
	else if (iter != vc_partner_ids.end() && enable_vc_option == 0) {
		vc_partner_ids.erase(receiver_id);
	}
}
void heartbeat_ping(chat_message& m) {
	uint8_t sender_id = 0;
	std::memcpy(&sender_id, m.body(), 1);
	if (sender_id != me.id) { std::cout << "heartbeat id fail match\n"; return; }
	chat_message msg;
	msg.set_message_type(message_type::heartbeat);
	std::memcpy(m.body(), &me.id, 1);
	msg.body_length(sizeof(me.id));
	msg.encode_header();
	write_ssl(msg);
}
void build_nonce(
	const std::array<uint8_t, 12>& iv_base,
	uint32_t counter,
	uint8_t nonce[12])
{
	std::memcpy(nonce, iv_base.data(), 8);
	nonce[8] = (counter >> 24) & 0xFF;
	nonce[9] = (counter >> 16) & 0xFF;
	nonce[10] = (counter >> 8) & 0xFF;
	nonce[11] = (counter) & 0xFF;
}

bool aes_gcm_encrypt(
	const uint8_t* key,
	const uint8_t* nonce,
	const uint8_t* plaintext,
	int plaintext_len,
	const uint8_t* aad,
	int aad_len,
	uint8_t* ciphertext,
	uint8_t* tag
) {
		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (!ctx)return false;
		if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
			return false;
		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
			return false;
		if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
			return false;
		int len = 0;
		if (aad && aad_len > 0){
			if (EVP_EncryptUpdate(ctx, NULL, &len, aad, aad_len) != 1)
				return false; 
		}
		if (EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) != 1)
			return false;
		int ciphertext_len = len;
		if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &len) != 1)
			return false;
		ciphertext_len += len;
		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1)
			return false;
		EVP_CIPHER_CTX_free(ctx);
		return true;
	}
	bool aes_gcm_decrypt(
		const uint8_t* key,
		const uint8_t* nonce,
		const uint8_t* ciphertext,
		int ciphertext_len,
		const uint8_t* aad,
		int aad_len,
		const uint8_t* tag,
		uint8_t* plaintext
	) {
		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (!ctx) { 
			//std::cout << "fail ctx\n";
			return false; }
		
		if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1)
		{
			//std::cout << "fail EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1\n";
			return false;
		}
		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1)
		{
			//std::cout << "fail EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1\n";
			return false;
		}
		if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
		{
			//std::cout << "fail EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1\n";
			return false;
		}
		int len = 0;
		if (aad && aad_len > 0) {
			if (EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len) != 1)
			{
			//	std::cout << "fail EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len) != 1\n";
				return false;
			}
		}
		if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1)
		{
			//std::cout << "fail EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) != 1\n";
			return false;
		}
		int plaintext_len = len;
		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1)
		{
			//std::cout << "fail EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1\n";
			return false;
		}
		int ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);
		EVP_CIPHER_CTX_free(ctx);
		//std::cout << "ret = " << ret << "\n";
		return ret > 0;
	}
	void store_voice_session_key(chat_message& m) {
		if (m.body_length() < audio_ctx.session_key.size() + audio_ctx.iv_base.size()) {
			std::cerr << "Invalid key message size\n";
			return;
		}
		std::memcpy(audio_ctx.session_key.data(), m.body(), audio_ctx.session_key.size());
		std::memcpy(audio_ctx.iv_base.data(), m.body() + audio_ctx.session_key.size(), audio_ctx.iv_base.size());
		audio_ctx.send_counter = 0;
	}
	void ok_shutdown(chat_message& m) {
		chat_message msg;
		msg.set_message_type(message_type::no_open_room);
		std::memcpy(m.body(), &me.id, 1);
		msg.body_length(sizeof(me.id));
		msg.encode_header();
		write_ssl(msg);

	}
	void process_msg_type(chat_message& m) {
		switch (m.msg_type) {
			case message_type::chat:
				msg_history.push_back(std::string(read_msg_.body(), read_msg_.body_length()));
				msg_rcvd = true;
				break;
			case message_type::name_challenge:
				store_voice_session_key(m);
				prompt_for_name = true;
				has_name = false;
				change_name = true;
				server_ping = true;
				awaiting_change_response = false;
				break;
			case message_type::name_approve:
				has_name = true;
				change_name = false;
				first_enter = true;
				awaiting_change_response = false;
				prompt_for_name = false;
				break;
			case message_type::particpants_request:
				add_participants(m);
				break;
			case message_type::ping:
				server_ping = true;
				state = client_state::connected;
				break;
			case message_type::authentication_request:
				send_authentication();
				break;
			case message_type::authentication_approve:
				authenticated = true;
				state = client_state::authenticated;
				session_token = decode_session_token(m);
				me.session_token = session_token;				
				send_start_room_request();
				break;
			case message_type::authentication_reject:
				session_token = "bad";
				authenticated = false;
				break;
			case message_type::reject_join:
				prompt_for_name = true;
				has_name = false;
				change_name = true;
				server_ping = true;
				awaiting_change_response = false;
				break;
			case message_type::send_id:
				read_id(m);
				audio_ctx.init_capture_rb();
				break;
			case message_type::recieve_vc_request: 
				receive_vc_request(m);
				break;			
			case message_type::reject_vc_request:
				vc_request_rejected(m);
				break;
			case message_type::accept_vc_request:
				vc_request_accepted(m);
				voice_enabled = true;
				break;
			case message_type::start_vc: {
				std::string token = decode_session_token(m);
				if (token == session_token) {
					std::cout << "session token validated. start_capture and playback\n";
					vc_room_id = 0;
					audio_ctx.start_capture();
					audio_ctx.start_playback();
					check_and_read_header_test();
				}
				break;
			}
			case message_type::bad_message: {
				session_token = "bad";
				authenticated = false;
				state = client_state::awaiting_connection;
				break;
			}
			case message_type::mic_test: {
				voice_enabled = true;
				mic_test = true;
				start_mic_test(m);
				break;
			}
			case message_type::end_chat: {
				remove_sender_from_chat(m);
				break;
			}
			case message_type::send_udp_port:{
				//std::cout << "receive send_udp_port\n";
				audio_ctx.add_stream(m);
				store_server_port(m);
				send_udp_port();
				//std::cout << "end receive send_udp_port\n";
				break;
			}
			case message_type::vc_partner_update: {
				//std::cout << "receive vc_partner_update\n";
				audio_ctx.add_stream(m);
				//std::cout << "end receive vc_partner_update\n";
				break;
			}
			case message_type::heartbeat: {
				std::cout << "send heartbeat\n";
				heartbeat_ping(m);
				break;
			}
			case message_type::version_check: {
				std::string error_message = std::string(m.body(), m.body_length());
				std::cout << error_message << "\n";
				state = client_state::bad_version;
				break;
			}
			case message_type::no_open_room: {
				std::string error_message = std::string(m.body(), m.body_length());
				state = client_state::no_open_room;
				break;
			}
			case message_type::voice_key: {
				//std::cout << "receive voice_key\n";
				store_voice_session_key(m);
			}
			default:
			{ break; }
		}
		//do_read_header();
		do_read_header_ssl();
	}
	void do_connect(const tcp::resolver::results_type& endpoints)
	{
		boost::asio::async_connect(*socket_, endpoints,
			[this](boost::system::error_code ec, tcp::endpoint) {
				std::cout << "async_connect running?\n";
				if (!ec) {
					retry_delay = 1;
					state = client_state::ready;
					timer_->cancel();
					do_read_header();
				}
				else {
					retry_delay = std::min(retry_delay * 2, max_delay);
					timer_->expires_after(std::chrono::seconds(retry_delay));
					timer_->async_wait([this](boost::system::error_code ec) {
						if (ec == boost::asio::error::operation_aborted) {
							return;
						}
						try_reconnect();
						});
				}
			});
	}
	void handle_reconnect_timer(boost::system::error_code ec) {
		//std::cout << "handle_reconenct_timer\n";
		if (!ec) {
			do_connect_ssl_test(endpoints);
		}
		else {
			//std::cout << "error handle reconnect timer: " << ec.message() << "\n";
			return;

		}
	}
	void do_reconnect() {
	//	std::cout << "do_reconnect\n";
		boost::system::error_code ec;
		ssl_socket_->lowest_layer().cancel(ec);
		ssl_socket_->lowest_layer().close(ec);
		ssl_socket_ = std::make_unique<boost::asio::ssl::stream<tcp::socket>>(io_context_, ssl_context_);
		ssl_socket_->set_verify_mode(boost::asio::ssl::verify_peer);
		ssl_socket_->set_verify_callback(
			std::bind(&chat_client::verify_certificate, this, std::placeholders::_1, std::placeholders::_2));
		//ssl_socket_->lowest_layer().open();
		state = client_state::connecting;
		//auto self = shared_from_this();
		steady_timer_.expires_after(boost::asio::chrono::milliseconds(500));
		//std::cout << "do timer wait\n";
		auto self = shared_from_this();
		steady_timer_.async_wait([self](const boost::system::error_code& ec) {
		//	std::cout << "start call to reconnect\n";
			self->handle_reconnect_timer(ec); });
		//std::cout << "timer wait done\n";
	}
	void handshake_test() {
		//std::cout << "start handshake\n";
		ssl_socket_->async_handshake(boost::asio::ssl::stream_base::client,
			[this](const boost::system::error_code& error) {
				if (!error) {
					//std::cout << "handshake succeed\n";
					state = client_state::ready;
					do_read_header_ssl();
				}
				else {
					do_reconnect();
					//std::cout << "handshake failed: " << error.message() << "\n";
				}
			});
	}
	void do_connect_ssl_test(const tcp::resolver::results_type& endpoints) {
		auto resolver = std::make_shared<tcp::resolver>(io_context_);
		//auto endpoints = resolver.resolve(argv[1], argv[2]);
		auto endpoints_new = resolver->resolve(ip, port);
		boost::asio::async_connect(ssl_socket_->lowest_layer(), endpoints_new,
			[this](const boost::system::error_code& error,
				const tcp::endpoint& /*endpoint*/) {
					//std::cout << "do_connect_ssl_test()\n";
					if (!error) {
						handshake_test();
					}
					else {
						do_reconnect();
					}
			});
	}

	void do_connect_ssl(const tcp::resolver::results_type& endpoints) {
		boost::asio::async_connect(ssl_socket_->lowest_layer(), endpoints,
			[this](const boost::system::error_code& error,
				const tcp::endpoint& /*endpoint*/) {
					if (!error) {
						//std::cout << "do connect ssl\n";
						retry_delay = 1;
						//do not set ready state until handshake is completed. it will send message to server expecting a tls handshake and cause
						// that handshake to fail.
						//state = client_state::ready; 
						timer_->cancel();
						handshake();
					}
					else {
						retry_delay = std::min(retry_delay * 2, max_delay);
						timer_->expires_after(std::chrono::seconds(retry_delay));
						timer_->async_wait([this](boost::system::error_code ec) {
							if (ec == boost::asio::error::operation_aborted) {
								return;
							}
							try_reconnect_ssl();
							});
						//std::cout << "connect failed: " << error.message() << "\n";
					}
		});
	}
	bool verify_certificate(bool preverified,
		boost::asio::ssl::verify_context& ctx) {
		char subject_name[256];
		X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
		X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);
		//std::cout << "verifying " << subject_name << "\n";
		return preverified;
	}
	void handshake() {
		//std::cout << "start handshake\n";
		ssl_socket_->async_handshake(boost::asio::ssl::stream_base::client,
			[this](const boost::system::error_code& error) {
				if (!error) {
					//std::cout << "handshake succeed\n";
					state = client_state::ready;
					do_read_header_ssl();
				}
				else {
					state = client_state::awaiting_connection;
					//std::cout << "handshake failed: " << error.message() << "\n";
				}
			});
	}
	void do_read_header() {
		std::cout << "do_read_header()\n";
		boost::asio::async_read(*socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this](boost::system::error_code ec, std::size_t/*length*/) {
				std::cout << "do_read_header async_read() running for some reason?\n";
				if (!ec && read_msg_.decode_header()) {	
					if (read_msg_.msg_type == message_type::particpants_request) {
						//return;
					}
					do_read_body();
				}
				else {
					//std::cout << "server disconnect header: " << ec.message() << "\n";
					//std::cout << "data is[" << read_msg_.data() << "]\n";
					state = client_state::awaiting_connection;
					socket_->close();
					msg_history.clear();
					participant_names.clear();
					participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			});
	}
	void do_read_header_ssl() {
		//std::cout << "do_read_header_ssl()\n";
		boost::asio::async_read(*ssl_socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this](boost::system::error_code ec, std::size_t) {
				//std::cout << "decode_header ssl()\n";
				if (!ec && read_msg_.decode_header()) {
					do_read_body_ssl();
				}
				else {
					//std::cout << "decode_header faifl\n";
					mic_test = false;
					me.vc_state = voice_chat_state::none;
					//std::cout << "state = " << state << "\n";
					if (state != client_state::bad_version && state != client_state::no_open_room) {
						state = client_state::awaiting_connection;
					}
					/*if (udp_socket && udp_socket->is_open()) {
						udp_socket->cancel(ec);
						udp_socket->close(ec);
					}	*/				
					udp_socket->cancel(ec);
					udp_socket->close(ec);

					udp_port_client = 0;
					/*if (ssl_socket_) {
						auto& sock = ssl_socket_->lowest_layer();
						if (sock.is_open()) {
							sock.cancel(ec);
							sock.close(ec);
						}
					}	*/		
					//ssl_socket_->lowest_layer().cancel();
					ssl_socket_->lowest_layer().close();
					//steady_timer_.cancel();
					msg_history.clear();
					participant_names.clear();
					participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			}
		);
	}
	void do_read_body() {
		std::cout << "do_read_body()\n";
		boost::asio::async_read(*socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this](boost::system::error_code ec, std::size_t /*length*/)
			{
				std::cout << "do_read_body running????\n";
				if (!ec) {
					std::string header = std::string(read_msg_.data(), chat_message::header_length);
					std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header[" << header << "] body [" << body << "]\n";
					process_msg_type(read_msg_);
				}
				else {
					//std::cout << "server disconnect body: " << ec.message() << "\n";
					state = client_state::awaiting_connection;
					socket_->close();
					msg_history.clear();
					participant_names.clear();
					participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			});
	}
	void do_read_body_ssl() {
		//std::cout << "do_read_body_ssl()\n";
		boost::asio::async_read(*ssl_socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this](boost::system::error_code ec, std::size_t) {
				if (!ec) {
					std::string header = std::string(read_msg_.data(), chat_message::header_length);
					std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header[" << header << "] body [" << body << "]\n";
					process_msg_type(read_msg_);
				}
				else {
					//std::cout << "server disconnect body: " << ec.message() << "\n";
					state = client_state::awaiting_connection;
					ssl_socket_->lowest_layer().close();
					msg_history.clear();
					participant_names.clear();
					participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			});
	}
	void do_write() {
		std::cout << "do_write()\n";
		boost::asio::async_write(*socket_,
			boost::asio::buffer(write_msgs_.front().data(),
				write_msgs_.front().length()),
			[this](boost::system::error_code ec, std::size_t/*length*/)
			{
				std::cout << "do_write async_write????\n";
				if (write_msgs_.front().msg_type == message_type::name_change_request) {
					//return;
				}
				std::string header = std::string(write_msgs_.front().data(), chat_message::header_length);
				std::string body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
				//std::cout << "write msg header[" << header << "] body [" << body << "]\n";

				if (!ec) {
					if (write_msgs_.front().msg_type == message_type::name_change_request) {						
						//return;
					}
					if (write_msgs_.front().msg_type == message_type::send_udp_port) {
						//std::cout << "client sending udp port info\n";
						//return;
					}


					if (write_msgs_.front().msg_type == message_type::ready_notification) {
						state = client_state::awaiting_authentication;
					}
					if (write_msgs_.front().msg_type == message_type::authentication_response) {
						std::string auth_body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
						state = client_state::awaiting_authentication;
					}
					write_msgs_.pop_front();
					if (!write_msgs_.empty()) {
						do_write();
					}
				}
				else {
					//std::cout << "server disconnect do_write: " << ec.message() << "\n";
					state = client_state::awaiting_connection;
					socket_->close();
					msg_history.clear();
					participant_names.clear();
					participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			}
		);
	}
	void do_write_ssl() {
		//std::cout << "do_write_ssl()\n";
		boost::asio::async_write(*ssl_socket_,
			boost::asio::buffer(write_msgs_.front().data(), write_msgs_.front().length()),
			[this](boost::system::error_code ec, std::size_t) {
				std::string header = std::string(write_msgs_.front().data(), chat_message::header_length);
				std::string body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
				//std::cout << "write msg header[" << header << "] body [" << body << "]\n";
				if (!ec) {
					if (write_msgs_.front().msg_type == message_type::ready_notification) {
						state = client_state::awaiting_authentication;
					}
					if (write_msgs_.front().msg_type == message_type::authentication_response) {
						std::string auth_body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
						state = client_state::awaiting_authentication;
					}
					write_msgs_.pop_front();
					if (!write_msgs_.empty()) {
						do_write_ssl();
					}
				}
				else {
					//std::cout << "server disconnect do_write_ssl: " << ec.message() << "\n";
					state = client_state::awaiting_connection;
					ssl_socket_->lowest_layer().close();
					msg_history.clear();
					participant_names.clear();
					participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			}
		);
	}
	void add_message(const std::string& msg) {
		msgs.push_back(msg);
		scroll_to_bottom = true;
	}
private:
	GLFWwindow* window;
	boost::asio::ssl::context& ssl_context_;
	std::shared_ptr<tcp::socket> socket_;
	std::unique_ptr <boost::asio::ssl::stream<tcp::socket>> ssl_socket_;//must come after socket_ and ssl_context_
	uint16_t udp_port_server;
	uint16_t udp_port_client;
	std::shared_ptr<udp::socket> udp_socket;
	chat_message read_msg_;
	chat_message_queue write_msgs_;
	tcp::resolver::results_type endpoints;
	bool scroll_to_bottom;
	int retry_delay = 1;
	int max_delay = 32;
	std::unique_ptr<boost::asio::steady_timer> timer_;
	boost::asio::steady_timer udp_timer_;
	ma_context ma_capture_context;
	ma_context ma_playback_context;
	std::array<uint8_t, 1500>recv_buffer_;
	boost::asio::steady_timer steady_timer_;

	chat_client(boost::asio::io_context& io_context,
		boost::asio::ssl::context& ssl_context,
		const tcp::resolver::results_type& endpoints, GLFWwindow* window)
		: io_context_(io_context), ssl_context_(ssl_context), socket_(std::make_shared<tcp::socket>(io_context)),
		udp_socket(std::make_shared<udp::socket>(io_context, udp::endpoint(udp::v4(), /*0*/ udp_port_number))), window(window), endpoints(endpoints),
		timer_(std::make_unique<boost::asio::steady_timer>(io_context)),
		udp_timer_{ udp_socket->get_executor() },
		audio_ctx{frame_size, sample_rate, this},
		me(), steady_timer_{io_context}, send_timer_{std::make_shared<boost::asio::steady_timer>(io_context)}
	{	

		ssl_socket_ = std::make_unique<boost::asio::ssl::stream<tcp::socket>>(io_context_, ssl_context_);
		ssl_socket_->set_verify_mode(boost::asio::ssl::verify_peer);
		ssl_socket_->set_verify_callback(
			std::bind(&chat_client::verify_certificate, this, std::placeholders::_1, std::placeholders::_2));
		me.id = 0;
		audio_ctx.init();
		audio_ctx.initialize_encoder();
		do_connect_ssl_test(endpoints);
		session_token = "";
		auto client_ep = udp_socket->local_endpoint();
		udp_port_client = client_ep.port();
		std::cout << "client udp_port = " << udp_port_client << "\n";
	}
};
//test
void playback_callback(ma_device* pDevice, void* pFramesOut, const void* pFramesIn, ma_uint32 frameCount) {
	if (pDevice == nullptr) { return; }
	//std::cout << "playback_callback_test() start\n";
	std::cout << "PLAYBACK: ch=" << pDevice->playback.channels
		<< " fmt=" << pDevice->playback.format
		<< " rate=" << pDevice->sampleRate << "\n";

	auto* ctx = static_cast<Audio_Context*>(pDevice->pUserData);
	const ma_uint32 out_ch = pDevice->playback.channels;
	const ma_uint32 total_samples = frameCount * out_ch;
	float* out = (float*)pFramesOut;
	std::fill(ctx->mix_buffer.begin(), ctx->mix_buffer.end(), 0);
	//std::fill(ctx->temp_buffer.begin(), ctx->temp_buffer.end(), 0);

	ma_result result;
	auto iter = ctx->vc_streams.begin();
	//std::cout << "vc_streams.count = " << ctx->vc_streams.size() << "\n";
	for (; iter != ctx->vc_streams.end(); ++iter) {
		//std::cout << "reading vc stream # " << static_cast<int>(iter->first) << "\n";

		rbs& stream = iter->second;

		ma_uint32 frames_to_read = frameCount;
		void* p_in = nullptr;
		ma_result result = ma_pcm_rb_acquire_read(
			&stream.playback_rb,
			&frames_to_read,
			&p_in
		);
		if (result == MA_SUCCESS && frames_to_read < frameCount) {
			//std::cout << "UNDERRUN: wanted " << frameCount
				//<< " got " << frames_to_read
				//<< " for talker " << int(iter->first) << "\n";
		}

		if (result != MA_SUCCESS || frames_to_read == 0) {
			continue;
		}
		float* in = (float*)p_in;
		float vol = 1.0f;
		auto it = ctx->c->participant_client_map.find(iter->first);
		if (it != ctx->c->participant_client_map.end()) {
			vol = it->second.output_volume;
		}

		float* temp = ctx->playback_temp_buffer.data();

		ctx->sound_controls.apply_gain_and_upmix(temp, in, frames_to_read, vol,
			ctx->network_channels, out_ch);
		ma_uint32 samples_read = frames_to_read * out_ch;
		ma_uint32 samples_needed = frameCount * out_ch;
		if (samples_read > total_samples) {
			samples_read = total_samples;
		}
		if (samples_read < samples_needed) {
			std::memset(temp + samples_read, 0,
				(samples_needed - samples_read) * sizeof(float));
		}
		for (uint32_t i = 0; i < samples_read; ++i) {
			ctx->mix_buffer[i] += temp[i];
		}
		//ma_pcm_rb_commit_read(&stream.playback_rb, frames_to_read);
		ma_pcm_rb_commit_read(&stream.playback_rb, frames_to_read);
		/*std::cout << "decoded_frames=" << decoded_frames
			<< " frames_to_read=" << frames_to_read
			<< " device_frameCount=" << frameCount << "\n";*/
	}
	std::memcpy(out, ctx->mix_buffer.data(), total_samples * sizeof(float));
}

void capture_callback(ma_device* pDevice, void* pFramesOut, const void* pFramesIn, ma_uint32 frameCount) {
	if (pDevice == nullptr || pFramesIn == nullptr) { return; }
	//std::cout << "capture_callback_test() start\n";
	auto* ctx = static_cast<Audio_Context*>(pDevice->pUserData);

	const uint32_t in_ch = pDevice->capture.channels;
	const uint32_t out_ch = ctx->network_channels;

	const float gain_db = std::pow(10.0f, gain * 0.45f);
	const float* src = ((const float*)pFramesIn);
	float* out = ctx->capture_temp_buffer.data();
	ma_result result;
	ma_uint32 framesWritten;
	framesWritten = 0;

	while (framesWritten < frameCount && ctx->running_capture) {
		void* pMappedBuffer;
		ma_uint32 framesToWrite = frameCount - framesWritten;
		result = ma_pcm_rb_acquire_write(&ctx->capture_ring_buffer, &framesToWrite, &pMappedBuffer);
		if (result != MA_SUCCESS || framesToWrite == 0) {
			//std::cerr << "acquire_write fail " << " size = " << sizeInBytes << "\n";
			break;
		}
		//std::cout << framesWritten + framesToWrite << " <= " << frameCount << "\n";
		//std::cout << ctx->capture_temp_buffer.size() << " >= " << framesToWrite * out_ch << "\n";

		ctx->sound_controls.apply_gain_and_upmix(
			out,
			src + framesWritten * in_ch,
			framesToWrite,
			gain_db,
			in_ch,
			out_ch
		);
		ma_uint32 playback_bpf = ma_get_bytes_per_frame(ctx->network_format, out_ch);
		std::memcpy(pMappedBuffer, out, framesToWrite * playback_bpf);
		result = ma_pcm_rb_commit_write(&ctx->capture_ring_buffer, framesToWrite);

		if (result != MA_SUCCESS) {
			//std::cout << "commit write fails\n";
			break;
		}
		framesWritten += framesToWrite;
		//std::cout << "capture_callback write " << framesToWrite << "\n";	
		ctx->c->schedule_check_and_send_test();
		//std::cout << "capture_callback_test() end\n";
	}
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
void draw_menu_bar(std::shared_ptr<chat_client>& c, GLFWwindow* window) {
	//MARKER1
	//std::cout << "draw_menu_bar start\n";
	if (ImGui::BeginMenuBar())
	{
		if (ImGui::BeginMenu("Options"))
		{
			first_enter = false;
			//TODO changing name while in voice chat crashes server
			if (ImGui::MenuItem("Change name")) {
				first_enter = true;
				change_name = true; 
				chat_message msg;
				msg.body_length(sizeof(uint8_t));
				msg.set_message_type(message_type::name_change_request);
				std::memcpy(msg.body(), &c->me.id, sizeof(uint8_t));
				msg.encode_header();
				c->write_ssl(msg);
			}
			if (ImGui::BeginMenu("Sound")) {
				if (ImGui::BeginMenu("Input")) {
					//char** items = new char* [capture_count];
					ImVec2 max{};
					ImVec2 current{};
					static int current_capture = 0;
					for (ma_uint32 i = 0; i < capture_devices_names.size(); i++) {
						current = ImGui::CalcTextSize(capture_devices_names[i]);
						if (current.x > max.x) {
							max.x = current.x;
						}
						if (current.y > max.y) {
							max.y = current.y;
						}
					}
					max.x += ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFontSize();
					ImGui::SetNextItemWidth(max.x);
					ImGui::Combo(" ", &current_capture, capture_devices_names.data(), capture_devices_names.size());
					if (current_capture != selected_capture) {
						selected_capture = current_capture;
						c->audio_ctx.restart_capture();
					}
					//delete[] items;
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Output")) {
					//char** items = new char* [playback_count];
					ImVec2 max{};
					ImVec2 current{};
					static int current_playback = 0;					
					for (ma_uint32 i = 0; i < playback_devices_names.size(); i++) {
						current = ImGui::CalcTextSize(playback_devices_names[i]);
						if (current.x > max.x) {
							max.x = current.x;
						}
						if (current.y > max.y) {
							max.y = current.y;
						}
					}
					max.x += ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFontSize();
					ImGui::SetNextItemWidth(max.x);
					ImGui::Combo(" ", &current_playback, playback_devices_names.data(), playback_devices_names.size());
					if (current_playback != selected_playback) {
						selected_playback = current_playback;
						c->audio_ctx.restart_playback();
					}
					//delete[] items;
					ImGui::EndMenu();
				}
				if (ImGui::Button("Refresh devices")) {
					c->audio_ctx.refresh_devices();
				}
				if (ImGui::BeginMenu("Controls")) {
					ImGui::SliderFloat("##Gain", & gain, 0.0f, 1.0f, "Mic Gain: %.001f");
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Exit")) {
				glfwSetWindowShouldClose(window, 1);
			}
			ImGui::EndMenu();
		}
		//std::cout << "draw_menu_bar end\n";
	}
	ImGui::EndMenuBar();
}
static bool init_focus = true;

const GLFWvidmode* mode;
int screenWidth;;
int screenHeight;

bool participant_header_enabled = false;

void draw_chat_window(std::shared_ptr<chat_client>& c, GLFWwindow* window) {
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	//std::cout << "draw chat window\n";
	ImVec2 display = ImGui::GetIO().DisplaySize;
	ImVec2 size(display.x, display.y);
	ImVec2 position((display.x - size.x) * 0.5f,
		(display.y - size.y) * 0.5f);
	ImGui::GetIO().FontGlobalScale = display.y * .0025f;
	ImGui::SetNextWindowPos(position);
	ImGui::SetNextWindowSize(size);
	//ImGui::SetNextWindowPos(viewport->Pos);
	//ImGui::SetNextWindowSize(viewport->Size);

	ImGuiWindowFlags flags = 
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_MenuBar;
	if (c->state == client_state::bad_version) {
		draw_error_window(c, window, size, position, "unsupported client version. update at magoogan.duckdns.org");
		return;
	}
	if (c->state == client_state::no_open_room) {
		draw_error_window(c, window, size, position, "no open rooms. try again later.");
		return;
	}
	if (c->state == client_state::awaiting_connection || c->state == client_state::connecting || !authenticated) {
		draw_disconnect_window(c, window, size, position);
		return;
	}
	if (c->state == client_state::ready || c->state == client_state::awaiting_authentication) {
		draw_start_connection_window(c, window, size, position);
		return;
	}
	if (ImGui::IsMouseClicked(0)) {
		first_enter = false;
	}
	//std::cout << "change_name = " << change_name << "awaiting_change_response = " << awaiting_change_response << "\n";
	if (change_name && !awaiting_change_response) {
		//std::cout << "change name\n";
		ImGui::SetNextWindowPos(position);
		ImGui::SetNextWindowSize(size);
		ImGui::Begin("Name", NULL, flags);
		//draw_menu_bar(c, window);
		ImGui::SetCursorPos(ImVec2(size.x * 0.35f, size.y * 0.5f));
		ImGui::SetNextItemWidth(size.x * 0.3f);
		std::string text;
		static std::string hint;
		hint = "Enter name";
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0) && first_enter)
		{
			ImGui::SetKeyboardFocusHere(0);
		}
		if (ImGui::InputTextWithHint("##Name", hint.c_str(), &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
			chat_message msg;
			uint8_t id = c->me.id;
			msg.body_length(sizeof(id) + text.size());
			msg.set_message_type(message_type::name_change_request);
			std::memcpy(msg.body(), &id, sizeof(uint8_t));
			std::memcpy(msg.body() + sizeof(uint8_t), text.c_str(), text.size());
			msg.encode_header();
			c->write_ssl(msg);
			hint = "";
			awaiting_change_response = true;
			text.clear();
		}
		ImGui::End();
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));

	ImGui::Begin("Main_Window", nullptr, flags);

	ImGui::PopStyleColor();
	if (!has_name) {
		ImGui::End();
		return;
	}

	draw_menu_bar(c, window);
	ImGui::SetCursorPos(ImVec2(size.x * 0.01f, size.y * 0.9f));
	ImGui::SetNextItemWidth(size.x * 0.8f);
	std::string text;

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0) && first_enter)
	//if (first_enter)
	{
	//	std::cout << "set focus 0\n";
		ImGui::SetKeyboardFocusHere(0);
	}
	//can't get focus back to this once lose it??
	//std::cout << "focus\n";
	if (ImGui::InputText("##Input Text", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
		chat_message msg;
		msg.body_length(text.length());
		msg.set_message_type(message_type::chat);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		c->write_ssl(msg);
		text.clear();
		//was_focused = true;
	}
	if (was_focused) {
		//std::cout << "set focus -1\n";
		ImGui::SetKeyboardFocusHere(-1);
		was_focused = false;
	}
	ImGui::SetCursorPos(ImVec2(size.x * 0.01f, size.y * 0.06f));
	ImGui::BeginChild("Output Text", ImVec2(size.x * 0.8f, size.y * 0.8f), ImGuiChildFlags_Borders);
	for (const auto& m : msg_history) {
		ImGui::TextWrapped("%s", m.c_str());
	}
	if (msg_rcvd) {
		ImGui::SetScrollHereY(0.999f);
		msg_rcvd = false;
	}
	ImGui::EndChild();

	ImGui::SetCursorPos(ImVec2(size.x * 0.82f, size.y * 0.06f));

	ImGuiWindowFlags scroll_flags = 0;
	scroll_flags |= ImGuiWindowFlags_NoTitleBar;
	scroll_flags |= ImGuiWindowFlags_NoMove;
	scroll_flags |= ImGuiWindowFlags_NoResize;
	scroll_flags |= ImGuiWindowFlags_NoCollapse;
	scroll_flags |= ImGuiWindowFlags_HorizontalScrollbar;

	ImGui::BeginChild("ChildL", ImVec2(size.x * 0.1f, size.y * 0.8f), ImGuiChildFlags_None, scroll_flags);
	ImGui::Text("Room");
	ImGui::Separator();
	std::unordered_map<uint8_t, participant_client_data>::iterator iter = c->participant_client_map.begin();
	for (; iter != c->participant_client_map.end(); ++iter) {
		ImGui::PushID(iter->second.p.id);
		std::string participant_name_label = std::string(iter->second.p.name); //+ "#" + std::to_string(iter->second.p.id);
		ImGui::Text(participant_name_label.c_str());
		ImGui::SameLine();
		if (ImGui::Checkbox("", &iter->second.enable_vc.first)) {
			chat_message msg;
			uint8_t sender_id = c->me.id;
			uint8_t receiver_id = iter->second.p.id;
			uint8_t enable_vc = iter->second.enable_vc.first ? 1 : 0;
			msg.body_length(sizeof(sender_id) + sizeof(receiver_id) + sizeof(enable_vc));
			msg.set_message_type(message_type::vc_status_check);
			std::memcpy(msg.body(), &sender_id, 1);
			std::memcpy(msg.body() + 1, &receiver_id, 1);
			std::memcpy(msg.body() + 2, &enable_vc, 1);
			msg.encode_header();
			c->write_ssl(msg);
			iter->second.ps = participant_state::sending_vc_request;
		}
		if (iter->second.enable_vc.first == 1) {
			ImGui::SliderFloat("vol", &iter->second.output_volume, 0.0f, 1.0f, "%.001f");
		}
		ImGui::PopID();
	}
	ImGui::EndChild();

	ImGui::End();
	//std::cout << "draw chat window end\n";

}
void enter_name_window(std::shared_ptr<chat_client>& c) {
	static bool no_resize = true;
	static bool no_move = true;
	static bool no_titlebar = true;

	ImGuiWindowFlags window_flags = 0;
	if (no_resize)          window_flags |= ImGuiWindowFlags_NoResize;
	if (no_move)          window_flags |= ImGuiWindowFlags_NoMove;
	if (no_titlebar)          window_flags |= ImGuiWindowFlags_NoTitleBar;
	const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 0, main_viewport->WorkPos.y + 0), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(main_viewport->Size.x, main_viewport->Size.y), ImGuiCond_Once);

	ImGui::Begin("Get Name", NULL, window_flags);
	ImGui::SetCursorPos(ImVec2(10, main_viewport->Size.y - 90.0f));
	std::string text;
	ImGui::Text("Name");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(300.0f);
	if (ImGui::InputTextWithHint("##NameInput","Enter name", & text, ImGuiInputTextFlags_EnterReturnsTrue)) {
		chat_message msg;
		msg.body_length(text.length());
		msg.set_message_type(message_type::chat);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		c->write_ssl(msg);
		text.clear();
	}
	ImGui::End();
}
void draw_error_window(std::shared_ptr<chat_client>& c, GLFWwindow* window, ImVec2& size, ImVec2& position, std::string msg) {
	static bool no_resize = true;
	static bool no_move = true;
	static bool no_titlebar = true;
	static bool no_menu = false;
	ImGuiWindowFlags window_flags = 0;
	if (no_resize)          window_flags |= ImGuiWindowFlags_NoResize;
	if (no_move)            window_flags |= ImGuiWindowFlags_NoMove;
	if (no_titlebar)        window_flags |= ImGuiWindowFlags_NoTitleBar;
	if (!no_menu)           window_flags |= ImGuiWindowFlags_MenuBar;
	ImGui::Begin("Error", NULL, window_flags);
	//draw_menu_bar(c, window);
	std::string text = msg;
	ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
	ImGui::SetCursorPos(ImVec2((size.x - text_size.x)*0.5f, (size.y - text_size.y) * 0.5f));
	ImGui::Text(text.c_str());
	ImGui::End();
}
void draw_start_connection_window(std::shared_ptr<chat_client>& c, GLFWwindow* window, ImVec2& size, ImVec2& position) {
	static bool no_resize = true;
	static bool no_move = true;
	static bool no_titlebar = true;
	static bool no_menu = false;
	ImGuiWindowFlags window_flags = 0;
	if (no_resize)          window_flags |= ImGuiWindowFlags_NoResize;
	if (no_move)            window_flags |= ImGuiWindowFlags_NoMove;
	if (no_titlebar)        window_flags |= ImGuiWindowFlags_NoTitleBar;
	if (!no_menu)           window_flags |= ImGuiWindowFlags_MenuBar;
	const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
	ImGui::Begin("Open", NULL, window_flags);
	//draw_menu_bar(c, window);
	ImGui::SetCursorPos(ImVec2(size.x * 0.4f, size.y * 0.5f));
	ImGui::SetNextItemWidth(size.x * 0.4f);
	std::string text = "Start connection";
	for (int i = 0; i < dots; i++) {
		text = text + ".";
	}
	dots = std::fmod(dots + static_cast<float>(delta_time.count()), 4);
	ImGui::Text(text.c_str());
	ImGui::End();
	if (c->state == client_state::ready) {
		c->send_version();
	}
}
void draw_disconnect_window(std::shared_ptr<chat_client>& c, GLFWwindow* window, ImVec2& size, ImVec2& position) {
	static bool no_resize = true;
	static bool no_move = true;
	static bool no_titlebar = true;
	static bool no_menu = false;
	ImGuiWindowFlags window_flags = 0;
	if (no_resize)          window_flags |= ImGuiWindowFlags_NoResize;
	if (no_move)            window_flags |= ImGuiWindowFlags_NoMove;
	if (no_titlebar)        window_flags |= ImGuiWindowFlags_NoTitleBar;
	if (!no_menu)           window_flags |= ImGuiWindowFlags_MenuBar;
	const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
	ImGui::Begin("Connecting", NULL, window_flags);
	//draw_menu_bar(c, window);
	ImGui::SetCursorPos(ImVec2(size.x * 0.4f, size.y * 0.5f));
	ImGui::SetNextItemWidth(size.x * 0.4f);
	std::string text = "Connecting";
	for (int i = 0; i < dots; i++) {
		text = text + ".";
	}
	dots = std::fmod(dots + static_cast<float>(delta_time.count()),4);
	ImGui::Text(text.c_str());
	ImGui::End();
	if(c->state == client_state::awaiting_connection){
		c->try_reconnect_ssl();
	}
}
template<typename Func>
void call_imgui(Func imgui_logic, GLFWwindow* window, std::shared_ptr<chat_client>& c) {
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();
	{
		imgui_logic(c, window);
	}
	ImGui::Render();
	int display_w, display_h;
	glfwGetFramebufferSize(window, &display_w, &display_h);
	glViewport(0, 0, display_w, display_h);
	glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	glfwSwapBuffers(window);
}
constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 1;


/*class SpectralSuppressor {
public:
	SpectralSuppressor(size_t frame_size):
		nfft(frame_size),
		cfg(kiss_fft_alloc(nfft, 0, nullptr, nullptr)),
		icfg(kiss_fft_alloc(nfft, 1, nullptr, nullptr)),
		noise_profile(nfft, 0.0f),
		alpha(0.95f),
		frame_buffer(frame_size)
	{
		if (!cfg || !icfg) {
			throw std::runtime_error("Failed to allocate KissFFT config");
		}
	}
	SpectralSuppressor(const SpectralSuppressor&) = delete;
	SpectralSuppressor& operator=(const SpectralSuppressor&) = delete;
	SpectralSuppressor(SpectralSuppressor&&) = delete;
	SpectralSuppressor& operator=(SpectralSuppressor&&) = delete;
	~SpectralSuppressor(){
		std::cout << "destructor\n";
		if(cfg) free(cfg);
		if(icfg) free(icfg);
	}
	void update_noise_profile(const int16_t* samples, float frame_count) {
		std::vector<kiss_fft_cpx> in(nfft), out(nfft);
		for (size_t i = 0; i < nfft; ++i) {
			float window = 0.5f * (1.0f - std::cos(2.0f * 3.14f * i / (nfft - 1)));
			in[i].r = samples[i] * window;
			in[i].i = 0;
		}
		for (size_t i = frame_count; i < nfft; ++i)
			in[i].r = 0.0f;

		kiss_fft(cfg, in.data(), out.data());
		for (size_t i = 0; i < nfft; ++i) {
			float mag = std::sqrt(out[i].r * out[i].r + out[i].i * out[i].i);
			mag = std::max(mag, 1.0f);
			noise_profile[i] = alpha * noise_profile[i] + (1.0f - alpha) * mag;
		}
	}*/
	/*void suppress(const int16_t* input, int16_t* output, ma_uint32 frame_count, float gain = 1.0f) {
		std::memcpy(frame_buffer.data(), input, frame_count * sizeof(int16_t));
		std::memset(frame_buffer.data() + frame_count, 0, (nfft - frame_count) * sizeof(int16_t));
		std::vector<kiss_fft_cpx> in(nfft), out(nfft);
		for (size_t i = 0; i < nfft; ++i) {
			in[i].r = input[i];
			in[i].i = 0;
		}
		kiss_fft(cfg, in.data(), out.data());

		for (size_t i = 0; i < nfft; ++i) {
			float mag = std::sqrt(out[i].r * out[i].r + out[i].i * out[i].i);
			float suppression = std::max(0.0f, mag - noise_profile[i]);
			float scale = suppression / (mag + 1e-6f);
			out[i].r *= scale;
			out[i].i *= scale;
		}

		kiss_fft(icfg, out.data(), in.data());
		
		for (size_t i = 0; i < nfft; ++i) {
			float sample = in[i].r / nfft * gain;
			//std::cout << "frame_buffer[" << i << "] " << frame_buffer[i] << "\n";
			frame_buffer[i] = static_cast<int16_t>(std::clamp(sample, -32768.0f, 32767.0f));
		}
		std::memcpy(output, frame_buffer.data(), frame_count * sizeof(int16_t));
	}
private:
	size_t nfft;
	//kiss_fft_cfg cfg;
	//kiss_fft_cfg icfg;
	std::vector<float> noise_profile;
	std::array<int16_t, 960> frame_buffer{};
	float alpha;
};*/


float compute_rms(const int16_t* samples, size_t count) {
	float sum = 0.0f;
	for (size_t i = 0; i < count; ++i)
		sum += samples[i] * samples[i];
	return std::sqrt(sum / count);
}

//HighPassFilter hp_filter(48000.0f, 100.0f);
//SpectralSuppressor suppressor(960);
constexpr float threshold = 100.0f;
constexpr float upper_threshold = 250.0f;
constexpr float lower_threshold = 100.0f;
//constexpr float frame_size = 960.0f;
void fade_out(ma_uint32 frame_count, Audio_Context* ctx, int16_t* out) {
	for (ma_uint32 i = 0; i < frame_count; ++i) {
		float fade_factor = 1.0f - static_cast<float>(ctx->fade_index) / ctx->fade_duration;
		fade_factor = std::clamp(fade_factor, 0.0f, 1.0f);

		float sample = static_cast<float>(out[i]) * fade_factor;
		out[i] = static_cast<int16_t>(std::clamp(sample, -32768.0f, 32767.0f));

		ctx->fade_index++;
		if (ctx->fade_index >= ctx->fade_duration) {
			ctx->fading_out = false;
			break;
		}
	}
}
//TODO add stream mixing
std::mutex mtx;
std::condition_variable cv;
bool shutdown_requested = false;

void on_notification(const ma_device_notification* pNotification)
{
	if (pNotification->type == ma_device_notification_type_stopped)
	{
		std::lock_guard<std::mutex> lock(mtx);
		shutdown_requested = true;
	}
	cv.notify_one();
}

void worker_thread() {
	while (running) {
		//do stuff
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	//exit thread
}

void test_digest() {
	EVP_MD_CTX* ctx = NULL;
	EVP_MD* sha256 = NULL;
	const unsigned char msg[] = {
		0x00, 0x01, 0x02, 0x03
	};
	unsigned int len = 0;
	unsigned char* outdigest = NULL;
	int ret = 1;

	/* Create a context for the digest operation */
	ctx = EVP_MD_CTX_new();
	if (ctx == NULL)
		goto err;

	/*
	 * Fetch the SHA256 algorithm implementation for doing the digest. We're
	 * using the "default" library context here (first NULL parameter), and
	 * we're not supplying any particular search criteria for our SHA256
	 * implementation (second NULL parameter). Any SHA256 implementation will
	 * do.
	 * In a larger application this fetch would just be done once, and could
	 * be used for multiple calls to other operations such as EVP_DigestInit_ex().
	 */
	sha256 = EVP_MD_fetch(NULL, "SHA256", NULL);
	if (sha256 == NULL)
		goto err;

	/* Initialise the digest operation */
	if (!EVP_DigestInit_ex(ctx, sha256, NULL))
		goto err;

	/*
	 * Pass the message to be digested. This can be passed in over multiple
	 * EVP_DigestUpdate calls if necessary
	 */
	if (!EVP_DigestUpdate(ctx, msg, sizeof(msg)))
		goto err;

	/* Allocate the output buffer */
	outdigest = static_cast<unsigned char*>(OPENSSL_malloc(EVP_MD_get_size(sha256)));
	if (outdigest == NULL)
		goto err;

	/* Now calculate the digest itself */
	if (!EVP_DigestFinal_ex(ctx, outdigest, &len))
		goto err;

	/* Print out the digest result */
	BIO_dump_fp(stdout, outdigest, len);

	ret = 0;

err:
	/* Clean up all the resources we allocated */
	OPENSSL_free(outdigest);
	EVP_MD_free(sha256);
	EVP_MD_CTX_free(ctx);
	if (ret != 0)
		ERR_print_errors_fp(stderr);
	//return ret;
}

//https://learn.microsoft.com/en-us/windows/win32/seccrypto/example-c-program-listing-the-certificates-in-a-store
void MyHandleError(std::string psz)
{
	std::cout << "An error occurred in the program. \n";
	std::cout << psz << "\n";
	std::cout << "Error number " << GetLastError() << "\n";
	std::cout << "Program terminating. \n";
	exit(1);
} // End of MyHandleError.

typedef struct _ENUM_ARG {
	BOOL fAll;
	BOOL fVerbose;
	DWORD dwFlags;
	const void* pvStoreLocationPara;
	HKEY hKeyBase;
} ENUM_ARG, *PENUM_ARG;

static BOOL WINAPI EnumPhyCallback(
	const void* pvSystemStore,
	DWORD dwFlags,
	LPCWSTR pwszStoreName,
	PCERT_PHYSICAL_STORE_INFO pStoreInfo,
	void* pvReserved,
	void* pvArg
);

static BOOL WINAPI EnumSysCallback(
	const void* pvSystemStore,
	DWORD dwFlags,
	PCERT_SYSTEM_STORE_INFO pStoreInfo,
	void* pvReserved,
	void* pvArg
);

static BOOL WINAPI EnumLocCallback(
	LPCWSTR pwszStoreLocation,
	DWORD dwFlags,
	void* pvReserved,
	void* pvArg
);
static BOOL GetSystemName(
	const void* pvSystemStore,
	DWORD dwFlags,
	PENUM_ARG pEnumArg,
	LPCWSTR* ppwszSystemName
) {
	*ppwszSystemName = NULL;
	if (pEnumArg->hKeyBase && 0 == (dwFlags & CERT_SYSTEM_STORE_RELOCATE_FLAG)) {
		std::cout << "failed => RELOCATE_FLAG not set in callback.\n";
		return FALSE;
	}
	else {
		if (dwFlags & CERT_SYSTEM_STORE_RELOCATE_FLAG) {
			PCERT_SYSTEM_STORE_RELOCATE_PARA pRelocatePara;
			if (!pEnumArg->hKeyBase) {
				MyHandleError("failed => RELOCATE_FLAG is set in callback");
			}
			pRelocatePara = (PCERT_SYSTEM_STORE_RELOCATE_PARA)
				pvSystemStore;
			if (pRelocatePara->hKeyBase != pEnumArg->hKeyBase) {
				MyHandleError("wrong hKeyBase passed to callback");
			}
			*ppwszSystemName = pRelocatePara->pwszSystemStore;
		}
		else {
			*ppwszSystemName = (LPCWSTR)pvSystemStore;
		}
	}
	return TRUE;
}

static BOOL WINAPI EnumPhyCallback(
	const void* pvSystemStore,
	DWORD dwFlags,
	LPCWSTR pwszStoreName,
	PCERT_PHYSICAL_STORE_INFO pStoreInfo,
	void* pvReserved,
	void* pvArg
) {
	PENUM_ARG pEnumArg = (PENUM_ARG)pvArg;
	LPCWSTR pwszSystemStore;
	if (GetSystemName(
		pvSystemStore,
		dwFlags,
		pEnumArg,
		&pwszSystemStore
	)) {
		std::wcout << pwszStoreName << "\n";
	}
	else {
		MyHandleError("GetSystemName failed.");
	}
	if (pEnumArg->fVerbose &&
		(dwFlags & CERT_PHYSICAL_STORE_PREDEFINED_ENUM_FLAG)) {
		std::cout << "	(implicitly created)\n";
	}
	return TRUE;
}

static BOOL WINAPI EnumSysCallback(
	const void* pvSystemStore,
	DWORD dwFlags,
	PCERT_SYSTEM_STORE_INFO pStoreInfo,
	void* pvReserved,
	void* pvArg
) {
	PENUM_ARG pEnumArg = (PENUM_ARG)pvArg;
	LPCWSTR pwszSystemStore;
	static int line_counter = 0;
	char x;

	if (line_counter++ > 5) {
		std::cout << "Enumeration of system store: press enter to continue.\n";
		scanf("%c", &x);
		line_counter = 0;
	}

	if (GetSystemName(pvSystemStore, dwFlags, pEnumArg, &pwszSystemStore)) {
		std::wcout << pwszSystemStore << "\n";
	}
	else {
		MyHandleError("GetSystemName failed.");
	}
	if (pEnumArg->fAll || pEnumArg->fVerbose) {
		dwFlags &= CERT_SYSTEM_STORE_MASK;
		dwFlags |= pEnumArg->dwFlags & ~CERT_SYSTEM_STORE_MASK;
		if (!CertEnumPhysicalStore(
			pvSystemStore,
			dwFlags,
			pEnumArg,
			EnumPhyCallback
		)) {
			DWORD dwErr = GetLastError();
			if (!(ERROR_FILE_NOT_FOUND == dwErr ||
				ERROR_NOT_SUPPORTED == dwErr)) {
				std::cout << "	CertEnumPhysicalStore\n";
			}
		}
	}
	return TRUE;
}

static BOOL WINAPI EnumLocCallback(
	LPCWSTR pwszStoreLocation,
	DWORD dwFlags,
	void* pvReserved,
	void* pvArg
) {
	PENUM_ARG pEnumArg = (PENUM_ARG)pvArg;
	DWORD dwLocationID = (dwFlags & CERT_SYSTEM_STORE_LOCATION_MASK) >>
		CERT_SYSTEM_STORE_LOCATION_SHIFT;
	static int linecount = 0;
	char x;

	if (linecount++ > 5) {
		std::cout << "enumeration of store locations: Press Enter to continue\n";
		scanf("%c", &x);
		linecount = 0;
	}

	std::wcout << "======= " << pwszStoreLocation << " ========\n";
	if (pEnumArg->fAll) {
		dwFlags &= CERT_SYSTEM_STORE_MASK;
		dwFlags |= pEnumArg->dwFlags & ~CERT_SYSTEM_STORE_LOCATION_MASK;
		CertEnumSystemStore(
			dwFlags,
			(void*)pEnumArg->pvStoreLocationPara,
			pEnumArg,
			EnumSysCallback
		);
	}
	return TRUE;
}
void add_windows_root_certs(boost::asio::ssl::context& ctx) {
	HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
	if (hStore == NULL) {
		std::cout << "the store wasn't opened\n";
		return;
	}
	std::cout << "the store was opened  :)\n";
	X509_STORE* store = X509_STORE_new();
	PCCERT_CONTEXT pContext = NULL;
	char pszNameString[256];
	while ((pContext = CertEnumCertificatesInStore(hStore, pContext)) != NULL) {
		X509* x509 = d2i_X509(NULL,
			(const unsigned char**)&pContext->pbCertEncoded,
			pContext->cbCertEncoded);
		if (x509 != NULL) {
			if (CertGetNameString(
				pContext,
				CERT_NAME_SIMPLE_DISPLAY_TYPE,
				0,
				NULL,
				pszNameString,
				128
			)) {
				std::cout << "certificate for " << pszNameString << "\n";
			}
			X509_STORE_add_cert(store, x509);
			X509_free(x509);
		}
	}
	CertFreeCertificateContext(pContext);
	CertCloseStore(hStore, 0);

	SSL_CTX_set_cert_store(ctx.native_handle(), store);
}
void test_list_certs() {
	DWORD dwExpectedError = 0;
	DWORD dwLocationID = CERT_SYSTEM_STORE_CURRENT_USER_ID;
	DWORD dwFlags = 0;
	CERT_PHYSICAL_STORE_INFO PhyStoreInfo;
	ENUM_ARG EnumArg;
	LPSTR pszStoreParameters = NULL;
	LPWSTR pwszStoreParameters = NULL;
	LPWSTR pwszSystemName = NULL;
	LPWSTR pwszPhysicalName = NULL;
	LPWSTR pwszStoreLocationPara = NULL;
	void* pvSystemName;
	void* pvStoreLocationPara;
	DWORD dwNameCnt = 0;
	LPCSTR pszTestName;
	HKEY hKeyRelocate = HKEY_CURRENT_USER;
	LPSTR pszRelocate = NULL;
	HKEY hKeyBase = NULL;

	memset(&PhyStoreInfo, 0, sizeof(PhyStoreInfo));
	PhyStoreInfo.cbSize = sizeof(PhyStoreInfo);
	PhyStoreInfo.pszOpenStoreProvider = (LPSTR)sz_CERT_STORE_PROV_SYSTEM_W;
	pszTestName = "Enum";
	pvSystemName = pwszSystemName;
	pvStoreLocationPara = pwszStoreLocationPara;

	memset(&EnumArg, 0, sizeof(EnumArg));
	EnumArg.dwFlags = dwFlags;
	EnumArg.hKeyBase = hKeyBase;

	EnumArg.pvStoreLocationPara = pvStoreLocationPara;
	EnumArg.fAll = TRUE;
	dwFlags &= ~CERT_SYSTEM_STORE_LOCATION_MASK;
	dwFlags |= (dwLocationID << CERT_SYSTEM_STORE_LOCATION_SHIFT) &
		CERT_SYSTEM_STORE_LOCATION_MASK;

	std::cout << "begin enumeration of store locations\n";
	if (CertEnumSystemStoreLocation(
		dwFlags,
		&EnumArg,
		EnumLocCallback
	)) {
		std::cout << "finished enumerating store locations\n";
	}
	else {
		MyHandleError("enumeration of locations failed");
	}
	std::cout << "begin enumeration of system stores\n";
	if (CertEnumSystemStore(
		dwFlags,
		pvStoreLocationPara,
		&EnumArg,
		EnumSysCallback
	)) {
		std::cout << "finished enumerating system stores\n";
	}
	else {
		MyHandleError("enumeration of system stores failed.");
	}
	std::cout << "\n\nenumerate the physical stores for the MY system store\n";
	if (CertEnumPhysicalStore(
		L"MY",
		dwFlags,
		&EnumArg,
		EnumPhyCallback
	)) {
		std::cout << "finished enumeration of physical stores.\n";
	}
	else {
		MyHandleError("enumeration of physical stores failed.");
	}
}
void test_open_cert_store() {
	HCERTSTORE hCertStore;
	PCCERT_CONTEXT pCertContext = NULL;
	char pszNameString[256];
	char pszStoreName[256];
	void* pvData;
	DWORD cbData;
	DWORD dwPropId = 0;

	std::string name;
	std::cout << "enter store name:";
	std::cin >> pszStoreName;
	std::cout << "the store name is " << name << "\n";
	if (hCertStore = CertOpenSystemStore(NULL, pszStoreName)) {
		std::cout << "the " << pszStoreName << " has been opened\n";
	}
	else {
		//std::cout << "the store wasn't opened\n";
		MyHandleError("The store was not opened");
		//return;
	}
	// pCertContext = NULL;
	while (pCertContext = CertEnumCertificatesInStore(
		hCertStore,
		pCertContext
	)) {
		if (CryptUIDlgViewContext(
			CERT_STORE_CERTIFICATE_CONTEXT,
			pCertContext,
			NULL,
			NULL,
			0,
			NULL)) {
			std::cout << "OK\n";
		}
		else {
			MyHandleError("UI failed");
			//continue;
		}
		if (CertGetNameString(
			pCertContext,
			CERT_NAME_SIMPLE_DISPLAY_TYPE,
			0,
			NULL,
			pszNameString,
			128
		)) {
			std::cout << "certificate for " << pszNameString << "\n";
		}
		else {
			MyHandleError("CertGetName failed.");
			//continue;
		}
		while (dwPropId = CertEnumCertificateContextProperties(
			pCertContext,
			dwPropId
		)) {
			std::cout << "property # " << dwPropId << " found->\n";
			switch (dwPropId) {
			case CERT_FRIENDLY_NAME_PROP_ID:
			{
				std::cout << "display name:\n";
				break;
			}
			case CERT_SIGNATURE_HASH_PROP_ID:
			{
				std::cout << "signature hash identifier:\n";
				break;
			}
			case CERT_KEY_PROV_HANDLE_PROP_ID:
			{
				std::cout << "KEY PROVE HANDLE\n";
				break;
			}
			case CERT_KEY_PROV_INFO_PROP_ID:
			{
				std::cout << "KEY PROV INFO PROP ID\n";
				break;
			}
			case CERT_SHA1_HASH_PROP_ID:
			{
				std::cout << "SHA1 HASH identifier\n";
				break;
			}
			case CERT_MD5_HASH_PROP_ID:
			{
				std::cout << "md5 hash identifier\n";
				break;
			}
			case CERT_KEY_CONTEXT_PROP_ID:
			{
				std::cout << "KEY CONTEXT PROP identifier\n";
				break;
			}
			case CERT_KEY_SPEC_PROP_ID:
			{
				std::cout << "KEY SPEC PROP identifier\n";
				break;
			}
			case CERT_ENHKEY_USAGE_PROP_ID:
			{
				std::cout << "ENHKEY USAGE PROP identifier\n";
				break;
			}
			case CERT_NEXT_UPDATE_LOCATION_PROP_ID:
			{
				std::cout << "NEXT UPDATE LOCATION PROP identifier\n";
				break;
			}
			case CERT_PVK_FILE_PROP_ID:
			{
				std::cout << "PVK FILE PROP identifier\n";
				break;
			}
			case CERT_DESCRIPTION_PROP_ID:
			{
				std::cout << "DESCRIPTION PROP identifier\n";
				break;
			}
			case CERT_ACCESS_STATE_PROP_ID:
			{
				std::cout << "ACCESS STATE PROP identifier\n";
				break;
			}
			case CERT_SMART_CARD_DATA_PROP_ID:
			{
				std::cout << "SMART CARD DATA PROP identifier\n";
				break;
			}
			case CERT_EFS_PROP_ID:
			{
				std::cout << "EFS PROP identifier\n";
				break;
			}
			case CERT_FORTEZZA_DATA_PROP_ID:
			{
				std::cout << "FORTEZZA DATA PROP identifier\n";
				break;
			}
			case CERT_ARCHIVED_PROP_ID:
			{
				std::cout << "ARCHIVED PROP identifier\n";
				break;
			}
			case CERT_KEY_IDENTIFIER_PROP_ID:
			{
				std::cout << "KEY IDENTIFIER PROP identifier\n";
				break;
			}
			case CERT_AUTO_ENROLL_PROP_ID:
			{
				std::cout << "AUTO ENROLL PROP identifier\n";
				break;
			}
			}
			if (CertGetCertificateContextProperty(
				pCertContext,
				dwPropId,
				NULL,
				&cbData
			)) {
			}
			else {
				std::cout << "call #1 to GetCertContextProperty failed\n";
				//continue;
			}
			if (pvData = (void*)malloc(cbData)) {
				//memory allocated. continue
			}
			else {
				std::cout << "memory allocation failed\n";
				//continue;
			}
			if (CertGetCertificateContextProperty(
				pCertContext,
				dwPropId,
				pvData,
				&cbData
			)) {
				//data retrieved
			}
			else {
				MyHandleError("call #2 failed");
				//continue;
			}
			std::cout << "the property content is " << pvData << "\n";
			free(pvData);
		}
	}
	if (!(pCertContext = CryptUIDlgSelectCertificateFromStore(
		hCertStore,
		NULL,
		NULL,
		NULL,
		CRYPTUI_SELECT_LOCATION_COLUMN,
		0,
		NULL
	))) {
		MyHandleError("select UI failed");
		//continue;
	}

	CertFreeCertificateContext(pCertContext);
	CertCloseStore(hCertStore, 0);
	std::cout << "function completed successfully\n";
}
int main(int argc, char* argv[])
{	 
	//add_windows_root_certs(ssl_io_ctx);
	//test_open_cert_store();
	//mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	//screenWidth = mode->width;
	//screenHeight = mode->height;
	const std::string cert_leaf = "isrg_cert.pem";
	std::filesystem::path cert_path = std::filesystem::current_path();
	//std::cout << "working directory: " << cert_path << "\n";
	cert_path.append(cert_leaf);
	std::string pem = cert_path.string();
	//std::cout << "pem = " << pem << "\n";

	msg_history.reserve(100);
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return 1;
	//GLFWmonitor* mon = glfwGetPrimaryMonitor();
	//const GLFWvidmode* mode = glfwGetVideoMode(mon);

	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	//glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);

	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	GLFWwindow* window = glfwCreateWindow((int)(640 * main_scale), (int)(400 * main_scale), "Chat", nullptr, nullptr);
	//GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, "Chat", nullptr, nullptr);
	if (window == nullptr)
		return 1;
	glfwMakeContextCurrent(window);
	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::cout << "Failed to initialize GLAD" << '\n';
		return -1;
	}
	glfwSwapInterval(1); // Enable vsync
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.ScaleAllSizes(main_scale);
	style.FontScaleDpi = main_scale;

	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	running = false;

	//test_list_certs();
	//test_open_cert_store();

	try {
		/*if (argc != 3) {
			std::cerr << "Usage: chat_client <host> <port> \n";
			return 1;
		}*/
		boost::asio::ssl::context ssl_context(boost::asio::ssl::context::tls_client);//tlsv12_client
		ssl_context.set_verify_mode(boost::asio::ssl::verify_peer);
		ssl_context.load_verify_file(pem);
		boost::asio::io_context io_context;		

		tcp::resolver resolver(io_context);
		//auto endpoints = resolver.resolve(argv[1], argv[2]);
		auto endpoints = resolver.resolve(ip, port);
		std::shared_ptr<chat_client> c = chat_client::create(io_context, ssl_context, endpoints, window);
		c->server_ip = ip;

		std::thread t([&io_context]() { 
			try {
				io_context.run();
			} 
			catch(const std::exception& e){
				std::cerr << "FATAL exception: " << e.what() << "\n";
			}
			});
		boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard(io_context.get_executor());
		char line[chat_message::max_body_length + 1];
		static ImGuiID last_active_id = 0;
		c->state = client_state::awaiting_connection;
		while (!glfwWindowShouldClose(window))
		{
			current_time = std::chrono::steady_clock::now();
			delta_time = current_time - last_time;
			last_time = current_time;

			glfwPollEvents();
			if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
			{
				ImGui_ImplGlfw_Sleep(10);
				continue;
			}
				call_imgui(draw_chat_window, window, c);
		}
		c->leave();
		c->msgs.clear();
		c->participant_names.clear();
		c->participants.clear();
		c->participant_map.clear();
		//std::cout << "close sockets\n";
		c->close();
		//std::cout << "stop capture/playback\n";
		c->audio_ctx.stop_capture();
		c->audio_ctx.stop_playback();
		//std::cout << "uninit_capture\n";
		if (c->audio_ctx.capture_init) {
			c->audio_ctx.uninit_capture();
		}
		//std::cout << "uninit_playback\n";
		if (c->audio_ctx.playback_init) {
			c->audio_ctx.uninit_playback();
		}
		//c->uninit_capture_rb();
		//c->uninit_playback_rb();
		//std::cout << "uninit vc_streams\n";
		c->audio_ctx.uninit_vc_streams();
		work_guard.reset();
		io_context.stop();
		t.join();
		
	}
	catch(std::exception& e){
		std::cerr << "Exception: " << e.what() << "\n";
	}

	//ma_device_uninit(&device);

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}