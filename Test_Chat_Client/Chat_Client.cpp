//C++17
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <random>
#include <sstream>
#include <fstream>
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
#include <ShlObj.h>           // SHGetKnownFolderPath
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
ma_uint32 SAMPLE_RATE = 48000;
constexpr int CHANNELS = 1;
constexpr ma_uint32 FRAME_SIZE = 960;
ma_format NETWORK_FORMAT = ma_format_f32;
ma_uint32 NETWORK_CHANNELS = 1;

const std::string client_version = "1.0";
const std::string ip = "159.89.49.248";
const std::string port = "5000";

std::string device_id = "";


//const size_t max_playback_size = 3840;
//const int buffer_size = 38400; //19200;
float db_threshold = 1.0f;

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

class sound_control {
public:
	void apply_gain(float* dst, const float* src, size_t count, float gain) {
		for (size_t i = 0; i < count; ++i) {
			float sample = src[i] * gain;
			if (sample > db_threshold) {
				sample = db_threshold;
			}
			if (sample < -db_threshold) {
				sample = -db_threshold;
			}
			dst[i] = sample;
		}
	}
	/*void apply_gain_and_upmix(float* dst, const float* src, size_t frame_count, float gain,
		uint32_t in_channels, uint32_t out_channels) {
		for (size_t f = 0; f < frame_count; ++f) {
			const float* in_frame = src + f * in_channels;
			float mono_value = in_frame[0] * gain;
			if (db_threshold > 0.0f) {
				float limit = db_threshold;
				mono_value = limit * tanh(mono_value / limit);
			}
			else {
				mono_value = 0;
			}
			float* out_frame = dst + f * out_channels;
			if (in_channels == 1) {
				for (uint32_t c = 0; c < out_channels; ++c) {
					out_frame[c] = mono_value;
				}
			}
			else {
				for (uint32_t c = 0; c < out_channels; ++c) {
					float s = in_frame[c % in_channels] * gain;
					if (db_threshold > 0.0f) {
						float limit = db_threshold;
						s = limit * tanh(s / limit);
					}
					else {
						s = 0;
					}
					out_frame[c] = s;
				}
			}
		}
	}*/
	void apply_gain_and_upmix(float* dst, const float* src, size_t frame_count,
		float gain, uint32_t in_channels, uint32_t out_channels)
	{
		// Prevent division-by-zero or crash bugs
		if (in_channels == 0 || out_channels == 0) return;

		// Linear limit for clipping (1.0f is digital maximum / 0 dBFS)
		const float limit = 1.0f;

		for (size_t f = 0; f < frame_count; ++f) {
			const float* in_frame = src + (f * in_channels);
			float* out_frame = dst + (f * out_channels);

			if (in_channels == 1) {
				// Standard Opus path: Input is Mono, upmix to output channels
				float mono_value = in_frame[0] * gain;

				// Only pay the heavy CPU cost of tanh if the signal actually clips
				if (mono_value > limit)       mono_value = limit * tanhf(mono_value / limit);
				else if (mono_value < -limit) mono_value = -limit * tanhf(-mono_value / limit);

				// Copy the processed mono sample to all output channels
				for (uint32_t c = 0; c < out_channels; ++c) {
					out_frame[c] = mono_value;
				}
			}
			else {
				// Fallback path: Input is Multi-channel (Stereo)
				for (uint32_t c = 0; c < out_channels; ++c) {
					// Map out_channels back to available in_channels gracefully
					float s = in_frame[c % in_channels] * gain;

					if (s > limit)       s = limit * tanhf(s / limit);
					else if (s < -limit) s = -limit * tanhf(-s / limit);

					out_frame[c] = s;
				}
			}
		}
	}

};
inline float hermite(float y0, float y1, float y2, float y3, float t)
{
	float c0 = y1;
	float c1 = 0.5f * (y2 - y0);
	float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
	float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
	return ((c3 * t + c2) * t + c1) * t + c0;
}
struct SimpleResampler {
	float prev2 = 0.0f;
	float prev1 = 0.0f;
	float  lastSample = 0.0f; 
	double phase = 0.0;  
	double ratio = 1.0;
	bool   primed = false;
};
size_t simple_resample(
	SimpleResampler& r,
	const float* in, size_t inFrames,
	size_t& inConsumed,
	float* out, size_t outCap)
{
	size_t outFrames = 0;
	size_t idx = 0;

	inConsumed = 0;

	if (inFrames == 0 || outCap == 0)
		return 0;

	if (!r.primed) {
		r.prev2 = in[0];
		r.prev1 = in[0];
		idx = 1;
		inFrames -= 1;
		r.primed = true;
	}

	while (inFrames > 0 && outFrames < outCap) {
		float y2 = in[idx];
		float y3 = (idx + 1 < inFrames) ? in[idx + 1] : y2;

		while (r.phase < 1.0 && outFrames < outCap) {
			float t = (float)r.phase;
			float y = hermite(r.prev2, r.prev1, y2, y3, t);
			out[outFrames++] = y;
			r.phase += r.ratio;
		}

		if (r.phase >= 1.0) {
			r.phase -= 1.0;

			r.prev2 = r.prev1;
			r.prev1 = y2;

			++idx;
			--inFrames;
		}
	}

	inConsumed = idx;
	return outFrames;
}class rbs {
public:
	ma_pcm_rb playback_rb;
	size_t size;
	OpusDecoder* decoder;
	ma_linear_resampler resampler;
	ma_linear_resampler_config resampler_cfg;

	rbs(ma_device& capture_device, ma_device& playback_device) : size{},
		decoder{ nullptr }, resampler{}
	{
		init_playback_rb(playback_device);
		initialize_decoder();
		resampler_cfg = ma_linear_resampler_config_init(NETWORK_FORMAT, NETWORK_CHANNELS,
			SAMPLE_RATE, playback_device.sampleRate);
		ma_linear_resampler_init(&resampler_cfg, nullptr, &resampler);

	}
	void initialize_decoder() {
		int error = 0;
		decoder = opus_decoder_create(
			SAMPLE_RATE,
			NETWORK_CHANNELS,
			&error
		);
	}
	bool init_playback_rb(ma_device& playback_device) {
		ma_uint32 bpf;
		ma_uint32 subBufferSizeInFrames = playback_device.playback.internalPeriodSizeInFrames * 5;
		bpf = ma_get_bytes_per_frame(playback_device.playback.format, playback_device.playback.channels);
		ma_result result;
		result = ma_pcm_rb_init(NETWORK_FORMAT, NETWORK_CHANNELS, subBufferSizeInFrames * bpf,
			NULL, NULL, &playback_rb);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize playback ring buffer\n";
			return false;
		}
		return true;
	}
	void uninit_playback_rb() {
		ma_pcm_rb_uninit(&playback_rb);
	}
};
class Audio_Context {
public:
	Audio_Context(size_t frame_size, chat_client* c_) : speaking{ false },
		current_gain{ 1.0f }, gain{ 20.0f },
		silence_frames{ 0 }, hangover_duration{ 10 }, fading_out{ false },
		fade_index{ 0 }, fade_duration{ 30 }, rms_smoothed{ 0.0f },
		encoder{ nullptr }, decoder{ nullptr },		
		sound_controls{},
		c{c_},
		capture_temp_buffer{},
		playback_temp_buffer{}
	{
		if (c == nullptr) { /*std::cout << "audio context c = nullptr\n";*/ }
		else { /*std::cout << "audio context not nullptr\n";*/ }
	}
	~Audio_Context() { /*std::cerr << "Audio Context destructor called\n";*/ }
	void init() {
		init_capture_context();
		init_playback_context();
		init_capture();
		init_playback();
		if (!playback_init || !capture_init) { return; }
		mix_buffer.resize(FRAME_SIZE * playback_device.playback.channels);
		playback_temp_buffer.resize(FRAME_SIZE * playback_device.playback.channels);
		capture_temp_buffer.resize(FRAME_SIZE * capture_device.capture.channels);
		bytes_per_sample_playback = ma_get_bytes_per_sample(NETWORK_FORMAT);
		bytes_per_frame_playback = ma_get_bytes_per_frame(NETWORK_FORMAT, NETWORK_CHANNELS);
	}
	void initialize_encoder() {
		int err = 0;
		encoder = opus_encoder_create(
			SAMPLE_RATE,
			NETWORK_CHANNELS,
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
		result = ma_context_get_devices(&playback_context, &playback_devices, &playback_count, NULL, NULL);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to get capture devices\n";
			return -1;
		}
		playback_devices_names.clear();
		playback_devices_names_storage.clear();
		for (ma_uint32 i = 0; i < playback_count; i++) {
			playback_devices_names_storage.emplace_back(playback_devices[i].name);
			playback_devices_names.push_back(playback_devices_names_storage[i].c_str());
		}
		if (playback_count > 0) {
			if (selected_playback >= playback_count) {
				return -1;
			}
		}
		deviceConfigPlayback = ma_device_config_init(ma_device_type_playback);
		deviceConfigPlayback.playback.format = NETWORK_FORMAT;
		deviceConfigPlayback.sampleRate = SAMPLE_RATE;
		deviceConfigPlayback.playback.channels = 0;
		deviceConfigPlayback.playback.pDeviceID = &playback_devices[selected_playback].id;
		deviceConfigPlayback.dataCallback = playback_callback;
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
		//std::cout << "uninit playback_device\n";
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
				return -1;
			}
		}
		deviceConfigCapture = ma_device_config_init(ma_device_type_capture);
		deviceConfigCapture.capture.format = NETWORK_FORMAT;
		deviceConfigCapture.sampleRate = SAMPLE_RATE;
		deviceConfigCapture.capture.channels = 0;
		deviceConfigCapture.capture.pDeviceID = &capture_devices[selected_capture].id;
		deviceConfigCapture.dataCallback = capture_callback;
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
		ma_device_uninit(&capture_device);
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
		//bpf = ma_get_bytes_per_frame(capture_device.capture.format, capture_device.capture.channels);
		ma_result result;
		result = ma_pcm_rb_init(NETWORK_FORMAT, NETWORK_CHANNELS, subBufferSizeInFrames, NULL, NULL, &capture_ring_buffer);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture ring buffer\n";
			return false;
		}
		return true;
	}
	void reinit_vc_streams() {
		auto iter = vc_streams.begin();
		for (; iter != vc_streams.end(); ++iter) {
			iter->second.uninit_playback_rb();
			iter->second.init_playback_rb(playback_device);
		}
	}
	void uninit_vc_streams() {
		auto iter = vc_streams.begin();
		for (; iter != vc_streams.end(); ++iter) {
			iter->second.uninit_playback_rb();
		}
	}
	void add_stream(chat_message& m) {
		uint8_t partner_id = 0;
		std::memcpy(&partner_id, m.body() + 1, 1);
		vc_streams.insert(std::make_pair(partner_id, rbs(capture_device, playback_device)));
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
			capture_devices_names_storage.emplace_back(capture_devices[i].name);
			capture_devices_names.push_back(capture_devices_names_storage[i].c_str());
		}
	}
	void refresh_playback_device_list() {
		ma_context_get_devices(&playback_context, &playback_devices, &playback_count, NULL, NULL);
		playback_devices_names.clear();
		playback_devices_names_storage.clear();
		for (ma_uint32 i = 0; i < playback_count; i++) {
			playback_devices_names_storage.emplace_back(playback_devices[i].name);
			playback_devices_names.push_back(playback_devices_names_storage[i].c_str());
		}
	}
	bool shutting_down = false;
	bool speaking;
	bool fading_out;
	float current_gain;
	float gain;
	//SpectralSuppressor spectral_suppressor;
	sound_control sound_controls;
	ma_uint32 silence_frames;
	ma_uint32 fade_index;
	const ma_uint32 fade_duration;
	const ma_uint32 hangover_duration; // ~200ms at 48kHz
	float rms_smoothed;
	OpusEncoder* encoder;
	OpusDecoder* decoder;
	ma_pcm_rb capture_ring_buffer;
	std::unordered_map<uint8_t, rbs> vc_streams;
	float input_float[FRAME_SIZE]{};
	uint8_t packet[4096]{};
	std::vector<float>mix_buffer;
	std::vector<float>playback_temp_buffer;
	std::vector<float>capture_temp_buffer;
	float encode_buffer[FRAME_SIZE];
	size_t encode_filled = 0;
	ma_uint32 bytes_per_sample_playback;
	ma_uint32 bytes_per_frame_playback;
	const ma_uint32 period_size_frames_playback = 960;
	ma_uint32 bytes_per_sample_capture;
	ma_uint32 bytes_per_frame_capture;
	chat_client* c;
	ma_device playback_device;
	ma_context playback_context;
	ma_device capture_device;
	ma_context capture_context;
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
	no_open_room = 9,
	checking_device_id = 10,
	checking_version = 11
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

	client_state state = client_state::awaiting_authentication;
	std::vector<std::string>participant_names;
	std::unordered_map<uint8_t, chat_participant>participant_map;
	std::unordered_map<uint8_t, participant_client_data>participant_client_map;
	chat_participant me;
	std::vector<std::string> msgs;
	std::unordered_set<uint8_t> requests_;
	bool authenticated = false;
	uint8_t vc_room_id;
	std::string server_ip;
	bool voice_enabled = false;
	boost::asio::ip::udp::endpoint server_endpoint;
	Audio_Context audio_ctx;
	boost::asio::io_context& io_context_;
	std::string session_token;//16 bytes/chars
	std::shared_ptr<boost::asio::steady_timer> send_timer_;
	bool stop_playback = false;
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
		do_reconnect();
	}
	void send_device_id() {
		//std::cout << "send_device_id " << device_id << "\n";
		chat_message m;
		m.set_message_type(message_type::device_id_send);
		std::memcpy(m.body(), device_id.c_str(), device_id.length());
		//std::memcpy(m.body(), "id", 2);
		m.body_length(device_id.length());
		m.encode_header();
		write_ssl(m);
	}

	void send_ready_notification() {
		chat_message msg;
		std::string text = "ready";
		msg.body_length(text.length());
		msg.set_message_type(message_type::ready_notification);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		write_ssl(msg);
	}
	void send_version() {
		chat_message version;
		version.body_length(client_version.length());
		version.set_message_type(message_type::version_check);
		std::memcpy(version.body(), client_version.c_str(), client_version.length());
		version.encode_header();
		write_ssl(version);
	}
	void write_ssl(const chat_message& msg) {
		boost::asio::post(io_context_,
			[this, msg]()
			{
				bool write_in_progress = !write_msgs_.empty();
				write_msgs_.push_back(msg);
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
				if (!write_in_progress) {
					do_write();
				}
			});
	}
	void close() {
		boost::asio::post(io_context_, [this]() {socket_->close(); });
	}
	void mix_and_write_to_playback_rb() {
		auto iter = audio_ctx.vc_streams.begin();
		for (; iter != audio_ctx.vc_streams.end(); ++iter) {
			ma_uint32 frames_available = 0;
			void* p_out = nullptr;
			while(frames_available < FRAME_SIZE) {
				ma_uint32 frames_written = frames_available;
				ma_result result = ma_pcm_rb_acquire_write(&iter->second.playback_rb, &frames_written, &p_out);
				if (result != MA_SUCCESS) {
					break;
				}
				result = ma_pcm_rb_commit_write(&iter->second.playback_rb, frames_written);
				if (result != MA_SUCCESS) {
					break;
				}
				frames_available += frames_written;
					//iter->second.playback_rb)
			}
		}
	}
	void schedule_playback_mixer() {
		mix_timer_.expires_after(boost::asio::chrono::milliseconds(20));
		mix_timer_.async_wait([this](const boost::system::error_code& ec) {
			mix_and_write_to_playback_rb();
			schedule_playback_mixer();
		});
	}

	void schedule_check_and_send_test() {
		auto self = shared_from_this();
		boost::asio::post(io_context_, [self] {
			self->send_timer_->expires_after(boost::asio::chrono::milliseconds(0));
			auto timer_cpy = self->send_timer_;
			self->send_timer_->async_wait([self, timer_cpy](const boost::system::error_code& ec) {
				self->check_and_send_test();
				self->schedule_check_and_send_test();
				});
			}
		);
	}
	void check_and_send_test() {
		if (!audio_ctx.running_capture) { return; }
		ma_uint32 frames_to_read = FRAME_SIZE;
		void* pOut = nullptr;
		ma_result result;
		result = ma_pcm_rb_acquire_read(&audio_ctx.capture_ring_buffer, &frames_to_read, &pOut);
		if (result != MA_SUCCESS || frames_to_read == 0) {
			return;
		}
		const float* pcm = (const float*)pOut;
		ma_uint32 consumed = 0;
		while (consumed < frames_to_read) {
			ma_uint32 can_copy = (ma_uint32)std::min(
				(size_t)(frames_to_read - consumed),
				FRAME_SIZE - audio_ctx.encode_filled
			);
			std::memcpy(audio_ctx.encode_buffer + audio_ctx.encode_filled,
				pcm + consumed, can_copy * sizeof(float));

			audio_ctx.encode_filled += can_copy;
			consumed += can_copy;
				
			if (audio_ctx.encode_filled == FRAME_SIZE) {
				//unsigned char opus_packet[4000];
				unsigned char opus_packet[1500];
				int encoded_bytes = opus_encode_float(
					audio_ctx.encoder,
					audio_ctx.encode_buffer,
					//frames_to_read,
					FRAME_SIZE,
					opus_packet,
					sizeof(opus_packet)
				);
				if (encoded_bytes <= 0) {
					//std::cout << "NETWORK SEND: encoded_bytes <= 0\n";
				}

				uint8_t nonce[12];
				build_nonce(audio_ctx.iv_base, audio_ctx.send_counter, nonce);

				uint8_t ciphertext[4000];
				uint8_t tag[16];

				std::shared_ptr<voice_chat_message> m = std::make_shared<voice_chat_message>();
				uint16_t body_len = (uint16_t)(4 + encoded_bytes + 16); //4 = send_counter, 16 = tag
				m->encode_header(session_token, body_len, vc_room_id, me.id, 0);
				const uint8_t* aad = (const uint8_t*)m->data();
				int aad_len = m->header_length;
				std::string header = std::string((char*)aad, aad_len);
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
					//std::cout << "NETWORK SEND: encrypt fail\n";
				}
				uint8_t* body = (uint8_t*)m->body();
				std::memcpy(body, &audio_ctx.send_counter, 4);
				std::memcpy(body + 4, ciphertext, encoded_bytes);
				std::memcpy(body + 4 + encoded_bytes, tag, 16);

				audio_ctx.send_counter++;
				auto buffer = boost::asio::buffer(m->data(), m->length());
				auto self = shared_from_this();
				audio_ctx.encode_filled = 0;
				//std::cout << "NETWORK SEND: " << encoded_bytes << " client port is " << udp_socket->local_endpoint() << "\n";
				udp_socket->async_send_to(buffer, server_endpoint,
					[this, self, /*size*/consumed](boost::system::error_code ec, std::size_t bytes_sent/*std::size_t bytes*/) {
						if (!ec) {
						}
						else {
							udp_socket->close();
						}
					}
				);
			}
		}
		ma_pcm_rb_commit_read(&audio_ctx.capture_ring_buffer, frames_to_read);
		//std::cout << "NETWORK SEND: commit_read = " << frames_to_read << "\n";
		return;	
	}
	void check_and_read_header_test() {
		auto self = shared_from_this();
		auto read_vc_msg_ = std::make_shared<voice_chat_message>();
		//std::cout << "NETWORK READ: heartbeat\n";
		udp_socket->async_receive_from(boost::asio::buffer(read_vc_msg_->data(), voice_chat_message::header_length + voice_chat_message::max_body_length), server_endpoint,
			[this, self, read_vc_msg_](boost::system::error_code ec, std::size_t bytes) {
				//std::cout << "NETWORK READ: async_receive_from\n";
				if (!self->audio_ctx.running_playback) {
					boost::asio::post(io_context_, [self] {
						self->check_and_read_header_test();
						});	
					//std::cout << "NETWORK READ: fail !running_playback\n";
					return;
				}
				if (!ec) {
					if (!read_vc_msg_->decode_header()) {
						boost::asio::post(io_context_, [self] {
							self->check_and_read_header_test();
							});		
						//std::cout << "NETWORK READ: fail decode header\n";
						return;
					}
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
		size_t body_len = read_vc_msg_->body_length();
		uint8_t* body = (uint8_t*)read_vc_msg_->body();
		uint8_t sender_id = read_vc_msg_->sender_id;
		if (body_len < 4 + 16) {
			//std::cout << "NETWORK READ: fail body_len < 20\n";
			return;
		}		
		uint32_t counter;
		std::memcpy(&counter, body, 4);

		uint8_t* ciphertext = body + 4;
		size_t ct_len = body_len - 4 - 16;
		uint8_t* tag = body + 4 + ct_len;

		uint8_t nonce[12];
		build_nonce(audio_ctx.iv_base, counter, nonce);

		const uint8_t* aad = (const uint8_t*)read_vc_msg_->data(); //additional authenticated data 
		int aad_len = read_vc_msg_->header_length;
		std::string header = std::string((char*)aad, aad_len);
		uint8_t opus_packet[1500];
		if (ct_len > sizeof(opus_packet)) {
			//std::cout << "NETWORK READ fail: cipher_text length > max opus_packet size\n";
			return;
		}
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
			//std::cout << "NETWORK READ: fail decrypt\n";
			return;
		}
		rbs& stream = audio_ctx.vc_streams.at(sender_id);
		std::vector<float> pcm_out(FRAME_SIZE);
		int decoded_frames = opus_decode_float(
			stream.decoder,
			opus_packet,
			(opus_int32)ct_len,
			pcm_out.data(),
			FRAME_SIZE,
			0
		);
		if (decoded_frames <= 0) {
			//std::cout << "NETWORK READ fail: decoded_frames <= 0\n";
			return;
		}
		/*if (decoded_frames < FRAME_SIZE) {
			std::memset(pcm_out.data() + decoded_frames, 0, (FRAME_SIZE - decoded_frames) * sizeof(float));
			decoded_frames = FRAME_SIZE;
		}*/
		ma_uint64 in_frames = decoded_frames;
		ma_uint64 out_frames;
		ma_linear_resampler_get_expected_output_frame_count(
			&stream.resampler, in_frames, &out_frames);
		std::vector<float>out_buffer(out_frames);
		ma_linear_resampler_process_pcm_frames(
			&stream.resampler,
			pcm_out.data(),
			&in_frames,
			out_buffer.data(),
			&out_frames
		);

		/*ma_uint32 max_frames = FRAME_SIZE * 10;
		ma_uint32 readable = ma_pcm_rb_available_read(&stream.playback_rb);
		if (readable > max_frames) {
			ma_pcm_rb_seek_read(&stream.playback_rb, readable - max_frames);
		}
		//ma_uint64 out_frames = decoded_frames;
		size_t avail = ma_pcm_rb_available_write(&stream.playback_rb);
		size_t needed = out_frames;
		if (avail < needed) {
			size_t drop = needed - avail;
			drop -= drop % FRAME_SIZE; 
			ma_pcm_rb_seek_read(&stream.playback_rb, drop);
		}*/
		ma_uint32 frames_to_write = (ma_uint32)out_frames;
		ma_uint32 frames_written = frames_to_write;
		void* pOut = nullptr;
		ma_result result = ma_pcm_rb_acquire_write(
			&stream.playback_rb,
			&frames_written,
			&pOut
		);
		if (result != MA_SUCCESS || frames_written == 0) {
			//std::cout << "NETWORK READ fail: frames_written == " << frames_written << 
				//" result = " << result << "\n";
			return;
		}
		ma_uint32 bpf = audio_ctx.bytes_per_frame_playback;
		size_t bytes_to_write = frames_written * bpf;
		//std::memcpy(pOut, pcm_out.data(), bytes_to_write);
		std::memcpy(pOut, out_buffer.data(), bytes_to_write);
		ma_pcm_rb_commit_write(&stream.playback_rb, frames_written);
		//std::cout << "NETWORK READ: commit frames_written = " << frames_written << "\n";
	}


	std::shared_ptr<boost::asio::steady_timer> retry_read_capture_timer;
	void add_participants(chat_message& m) {
		auto iter = participant_client_map.find(me.id);
		std::pair<bool, bool> enable_vc{};
		if (iter != participant_client_map.end()) {
			enable_vc = iter->second.enable_vc;
		}
		participant_map.clear();
		participant_client_map.clear();
		if (m.body_length() < sizeof(uint8_t)) { return; }
		uint8_t ps = 0;
		char* ptr = m.body();
		std::memcpy(&ps, ptr, sizeof(uint8_t));
		ptr += sizeof(uint8_t);
		char* end = m.body() + m.body_length();
		std::unordered_map<uint8_t, chat_participant> temp_participant_map{};
		std::unordered_map<uint8_t, participant_client_data> temp_client_data_map{};
		for (int i = 0; i < ps; i++) {
			if (ptr >= end) { break; }
			chat_participant p{};
			uint8_t len = 0;
			std::memcpy(&len, ptr, sizeof(uint8_t));
			ptr += sizeof(uint8_t);
			size_t size = p.deserialize(ptr);
			ptr += len;
			participant_client_data pcd{ p };
			if (p.id == me.id) { pcd.enable_vc = enable_vc; }
			participant_map.emplace(p.id, p);
			participant_client_map.emplace(p.id,pcd);
		}
	}
	void send_authentication() {
		//std::cout << "send auth\n";
		chat_message auth;		
		auth.body_length(key.length());
		auth.set_message_type(message_type::authentication_response);
		std::memcpy(auth.body(), key.data(), key.length());
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
	}
	void store_server_port(chat_message& m) {
		uint8_t client_id = 0;
		uint8_t partner_id = 0;
		udp_port_server = 0;
		std::memcpy(&client_id, m.body(), 1);
			//std::cout << "client_id[" << static_cast<int>(client_id) << "]"
				//<< "\nme.id[" << static_cast<int>(me.id) << "]\n";
		std::memcpy(&partner_id, m.body() + 1, 1);
		//std::cout << "partner_id[" << static_cast<int>(partner_id) << "]";
		std::memcpy(&udp_port_server, m.body() + 2, 2);
				//std::cout << "udp_port_server[" << static_cast<int>(udp_port_server) << "]\n";
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
void remove_sender_from_chat(chat_message& m) {
	uint8_t sender_id;
	std::memcpy(&sender_id, m.body(), sizeof(sender_id));
	//std::cout << "remove[" << static_cast<int>(sender_id) << "] from chat\n";
	if (participant_map.find(sender_id) != participant_map.end())
	{
		//std::cout << "erase [" << static_cast<int>(sender_id) << "] from participant_map\n";
		participant_map.erase(sender_id);
	}
	if (participant_client_map.find(sender_id) != participant_client_map.end()) {
		participant_client_map.erase(sender_id);
		//std::cout << "erase [" << static_cast<int>(sender_id) << "] from participant_client_map\n";
	}
}
//not using this
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
void udp_heartbeat_ping() {
	//std::cout << "udp_heartbeat_ping\n";
	std::shared_ptr<voice_chat_message> m = std::make_shared<voice_chat_message>();
	uint16_t body_len = (uint16_t)(0); 
	m->encode_header(session_token, body_len, vc_room_id, me.id, 1);
	auto buffer = boost::asio::buffer(m->data(), m->length());
	udp_socket->async_send_to(buffer, server_endpoint,
		[this](boost::system::error_code ec, std::size_t bytes_sent/*std::size_t bytes*/) {
		}
	);
	udp_heartbeat_timer_.expires_after(std::chrono::seconds(5));
	udp_heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
		udp_heartbeat_ping();
		});

}
void start_udp_heartbeat_ping() {
	udp_heartbeat_ping();
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
				state = client_state::checking_device_id;
				send_device_id();
			//	std::cout << "authentication approved send device\n";

				//send_start_room_request();
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
				break;
			case message_type::accept_vc_request:
				//vc_request_accepted(m);
				voice_enabled = true;
				break;
			case message_type::start_vc: {
				std::string token = decode_session_token(m);
				if (token == session_token) {
					vc_room_id = 0;
					audio_ctx.start_capture();
					audio_ctx.start_playback();
					start_udp_heartbeat_ping();
					schedule_check_and_send_test();
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
				audio_ctx.add_stream(m);
				break;
			}
			case message_type::heartbeat: {
				//std::cout << "send heartbeat\n";
				heartbeat_ping(m);
				break;
			}
			case message_type::version_check: {
				std::string error_message = std::string(m.body(), m.body_length());
				std::cout << error_message << "\n";
				state = client_state::bad_version;
				break;
			}
			case message_type::version_approve: {
				state = client_state::ready;
				send_start_room_request();
				break;
			}
			case message_type::no_open_room: {
				std::cout << "process msg: no open room!\n";
				std::string error_message = std::string(m.body(), m.body_length());
				state = client_state::no_open_room;
				break;
			}
			case message_type::voice_key: {
				//std::cout << "receive voice_key\n";
				store_voice_session_key(m);
				break;
			}
			case message_type::device_id_approve: {
				state = client_state::checking_version;
				send_version();
				break;
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
					//state = client_state::ready;
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
	//	return;
		std::cout << "do_reconnect\n";
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
		//std::cout << "start call to reconnect\n";
			self->handle_reconnect_timer(ec); });
		//std::cout << "timer wait done\n";
	}
	void handshake_test() {
		//std::cout << "start handshake\n";
		ssl_socket_->async_handshake(boost::asio::ssl::stream_base::client,
			[this](const boost::system::error_code& error) {
				if (!error) {
					//std::cout << "handshake succeed\n";
					//state = client_state::ready;
					do_read_header_ssl();
					send_authentication();
				}
				else {
					do_reconnect();
					std::cout << "handshake failed: " << error.message() << "\n";
				}
			});
	}
	void do_connect_ssl_test(const tcp::resolver::results_type& endpoints) {
		//std::cout << "do_connect_ssl_test()\n";
		auto resolver = std::make_shared<tcp::resolver>(io_context_);
		//auto endpoints = resolver.resolve(argv[1], argv[2]);
		auto endpoints_new = resolver->resolve(ip, port);
		boost::asio::async_connect(ssl_socket_->lowest_layer(), endpoints_new,
			[this](const boost::system::error_code& error,
				const tcp::endpoint& /*endpoint*/) {
					//std::cout << "do_connect_ssl_test() lambda\n";
					if (!error) {
						
						handshake_test();
					}
					else {	
						//std::cout << "do_ssl_connect_test fail, do reconnect ec = " << error.message() << "\n";
						do_reconnect();
					}
			});
	}
	void do_connect_ssl(const tcp::resolver::results_type& endpoints) {
		boost::asio::async_connect(ssl_socket_->lowest_layer(), endpoints,
			[this](const boost::system::error_code& error,
				const tcp::endpoint& /*endpoint*/) {
					if (!error) {
						std::cout << "do connect ssl\n";
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
					//state = client_state::ready;
					do_read_header_ssl();
				}
				else {
					state = client_state::awaiting_connection;
					do_reconnect();
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
					//participants.clear();
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
			//	std::cout << "decode_header ssl()\n";

				if (!ec && read_msg_.decode_header()) {
					//std::cout << "!ec = " << ec.message() << "\n";
					//std::string header = std::string(read_msg_.data(), chat_message::header_length);
					//std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header success[" << header << "] body [" << body << "]\n";

					do_read_body_ssl();
				}
				else {
				
					//std::cout << "decode_header fail or ec: " << ec.message() << "\n";
					//std::string header = std::string(read_msg_.data(), chat_message::header_length);
					//std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header fail[" << header << "] body [" << body << "]\n";
					mic_test = false;
					me.vc_state = voice_chat_state::none;
					//std::cout << "state = " << state << "\n";
					if (state != client_state::bad_version && state != client_state::no_open_room) {
						state = client_state::awaiting_connection;
						do_reconnect();
					}
					udp_socket->cancel(ec);
					udp_socket->close(ec);
					udp_port_client = 0;
					msg_history.clear();
					participant_names.clear();
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
					//participants.clear();
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
					std::cout << "server disconnect body: " << ec.message() << "\n";
					state = client_state::awaiting_connection;
					do_reconnect();
					msg_history.clear();
					participant_names.clear();
					//participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			});
	}
	void do_write() {
		boost::asio::async_write(*socket_,
			boost::asio::buffer(write_msgs_.front().data(),
				write_msgs_.front().length()),
			[this](boost::system::error_code ec, std::size_t/*length*/)
			{
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
					//participants.clear();
					participant_map.clear();
					participant_client_map.clear();
				}
			}
		);
	}
	void do_write_ssl() {
	//	std::cout << "do_write_ssl()\n";
		boost::asio::async_write(*ssl_socket_,
			boost::asio::buffer(write_msgs_.front().data(), write_msgs_.front().length()),
			[this](boost::system::error_code ec, std::size_t) {
				//std::string header = std::string(write_msgs_.front().data(), chat_message::header_length);
				//std::string body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
				//std::cout << "write msg header[" << header << "] body [" << body << "]\n";
				if (!ec) {
					/*if (write_msgs_.front().msg_type == message_type::ready_notification) {
						state = client_state::awaiting_authentication;
					}*/
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
					std::cout << "server disconnect do_write_ssl: " << ec.message() << "\n";
					state = client_state::awaiting_connection;
					do_reconnect();
					msg_history.clear();
					participant_names.clear();
					//participants.clear();
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
	boost::asio::steady_timer mix_timer_;
	boost::asio::steady_timer udp_heartbeat_timer_;

	chat_client(boost::asio::io_context& io_context,
		boost::asio::ssl::context& ssl_context,
		const tcp::resolver::results_type& endpoints, GLFWwindow* window)
		: io_context_(io_context), ssl_context_(ssl_context), socket_(std::make_shared<tcp::socket>(io_context)),
		udp_socket(std::make_shared<udp::socket>(io_context, udp::endpoint(udp::v4(), udp_port_number))), window(window), endpoints(endpoints),
		timer_(std::make_unique<boost::asio::steady_timer>(io_context)),
		udp_timer_{ udp_socket->get_executor() },
		audio_ctx{FRAME_SIZE, this},
		me(), steady_timer_{io_context}, send_timer_{std::make_shared<boost::asio::steady_timer>(io_context)}, 
		mix_timer_{ boost::asio::steady_timer(io_context) },
		udp_heartbeat_timer_{io_context}
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
		//std::cout << "UDP open: " << udp_socket->is_open() << "\n";
		//std::cout << "UDP bound to: " << udp_socket->local_endpoint() << "\n";
	}
};
void playback_callback(ma_device* pDevice, void* pFramesOut, const void* pFramesIn, ma_uint32 frameCount) {
	if (pDevice == nullptr) { return; }
	auto* ctx = static_cast<Audio_Context*>(pDevice->pUserData);
	const ma_uint32 out_ch = pDevice->playback.channels;
	const ma_uint32 total_samples = frameCount * out_ch;
	float* out = (float*)pFramesOut;
	std::fill(ctx->mix_buffer.begin(), ctx->mix_buffer.end(), 0);
	ma_result result;
	auto iter = ctx->vc_streams.begin();
	for (; iter != ctx->vc_streams.end(); ++iter) {
		rbs& stream = iter->second;
		ma_uint32 frames_to_read = frameCount;
		void* p_in = nullptr;
		ma_result result = ma_pcm_rb_acquire_read(
			&stream.playback_rb,
			&frames_to_read,
			&p_in
		);
		if (result == MA_SUCCESS && frames_to_read < frameCount) {
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
			NETWORK_CHANNELS, out_ch);
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
		ma_pcm_rb_commit_read(&stream.playback_rb, frames_to_read);
		//std::cout << "PLAYBACK_CALLBACK: frames_to_read = " << frames_to_read << "\n";
	}
	std::memcpy(out, ctx->mix_buffer.data(), total_samples * sizeof(float));
}

void capture_callback(ma_device* pDevice, void* pFramesOut, const void* pFramesIn, ma_uint32 frameCount) {
	if (pDevice == nullptr || pFramesIn == nullptr) { return; }
	auto* ctx = static_cast<Audio_Context*>(pDevice->pUserData);

	const uint32_t in_ch = pDevice->capture.channels;
	const uint32_t out_ch = NETWORK_CHANNELS;

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
			break;
		}
		ctx->sound_controls.apply_gain_and_upmix(
			out,
			src + framesWritten * in_ch,
			framesToWrite,
			gain_db,
			in_ch,
			out_ch
		);
		ma_uint32 playback_bpf = ma_get_bytes_per_frame(NETWORK_FORMAT, out_ch);
		std::memcpy(pMappedBuffer, out, framesToWrite * playback_bpf);
		result = ma_pcm_rb_commit_write(&ctx->capture_ring_buffer, framesToWrite);

		if (result != MA_SUCCESS) {
			break;
		}
		framesWritten += framesToWrite;
		//std::cout << "CAPTURE_CALLBACK: framesWritten = " << framesWritten << "\n";
		//ctx->c->schedule_check_and_send_test();
	}
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
void draw_menu_bar(std::shared_ptr<chat_client>& c, GLFWwindow* window) {
	if (ImGui::BeginMenuBar())
	{
		if (ImGui::BeginMenu("Options"))
		{
			first_enter = false;
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
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Output")) {
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
					ImGui::SliderFloat("Gain", & gain, 0.0f, 1.0f, "##%.004f");
					//ImGui::SliderFloat("Threshold", &db_threshold, 0.0f, 1.0f, "##%.004f");
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
				//
			}
			if (ImGui::MenuItem("Exit")) {
				glfwSetWindowShouldClose(window, 1);
			}
			ImGui::EndMenu();
		}
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
	//std::cout << "c->state = " << static_cast<int>(c->state) << "\n";
	//std::cout << "1\n";
	if (c->state == client_state::awaiting_authentication || c->state == client_state::checking_device_id || c->state == client_state::checking_version) {
		draw_start_connection_window(c, window, size, position);
		return;
	}
	//std::cout << "2\n";

	if (c->state == client_state::awaiting_connection || c->state == client_state::connecting || !authenticated) {
		draw_disconnect_window(c, window, size, position);
		return;
	}
	//std::cout << "3\n";

	if (c->state == client_state::bad_version) {
		draw_error_window(c, window, size, position, "unsupported client version. update at magoogan.duckdns.org");
		return;
	}
	//std::cout << "4\n";

	if (c->state == client_state::no_open_room) {
		draw_error_window(c, window, size, position, "no open rooms. try again later.");
		return;
	}
	//std::cout << "5\n";

	if (c->state != client_state::ready) { 
		draw_disconnect_window(c, window, size, position);
		return; 
	}
	//std::cout << "6\n";

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
		ImGui::Text(iter->second.p.name.data());
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
			ImGui::SliderFloat("Vol", &iter->second.output_volume, 0.0f, 3.0f, "##%.04f");
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
constexpr float threshold = 100.0f;
constexpr float upper_threshold = 250.0f;
constexpr float lower_threshold = 100.0f;
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

//https://www.daniweb.com/programming/software-development/threads/475634/creating-a-txt-file-and-saving-it-to-a-desired-location
bool get_folder_path(std::string& path) {
	PWSTR p = nullptr;	
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
		std::filesystem::path base = std::filesystem::path(p);
		CoTaskMemFree(p);
		std::filesystem::path folder = base / "Test_Chat_Client";
		std::filesystem::create_directories(folder);
		std::filesystem::path out = folder / "device_id.txt";
		path = out.string();
		return true;
	}
	return false;
}
//https://stackoverflow.com/questions/24365331/how-can-i-generate-uuid-in-c-without-using-boost-library
bool create_device_id(std::string& d_id) {
	std::string path = "";
	if (!get_folder_path(path)) { return false; }
	//std::cout << "path = " << path << "\n";
	std::ifstream in{ path };
	if (in.good()) {
		std::string id;
		std::getline(in, id);
		if (!id.empty()) {
			d_id = id;
			return true;
		}
	}
	std::random_device              rd;
	std::mt19937                    gen(rd());
	std::uniform_int_distribution<> dis(0, 15);
	std::uniform_int_distribution<> dis2(8, 11);
	std::stringstream ss;
	int i;
	ss << std::hex;
	for (i = 0; i < 8; i++) {
		ss << dis(gen);
	}
	ss << "-";
	for (i = 0; i < 4; i++) {
		ss << dis(gen);
	}
	ss << "-4";
	for (i = 0; i < 3; i++) {
		ss << dis(gen);
	}
	ss << "-";
	ss << dis2(gen);
	for (i = 0; i < 3; i++) {
		ss << dis(gen);
	}
	ss << "-";
	for (i = 0; i < 12; i++) {
		ss << dis(gen);
	};
	std::ofstream out(path);
	out << ss.str();
	d_id = ss.str();
	return true;
}

int main(int argc, char* argv[])
{	 
	const std::string cert_leaf = "isrg_cert.pem";
	std::filesystem::path cert_path = std::filesystem::current_path();
	//std::cout << "cert_path = " << cert_path << "\n";
	cert_path.append(cert_leaf);
	std::string pem = cert_path.string();
	msg_history.reserve(100);
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return 1;
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	GLFWwindow* window = glfwCreateWindow((int)(640 * main_scale), (int)(400 * main_scale), "Chat", nullptr, nullptr);
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

	if (!create_device_id(device_id)) {
		std::cerr << "fail id\n";
		return -1;
	}

	try {
		boost::asio::ssl::context ssl_context(boost::asio::ssl::context::tls_client);//tlsv12_client
		ssl_context.set_verify_mode(boost::asio::ssl::verify_peer);
		ssl_context.load_verify_file(pem);
		boost::asio::io_context io_context;		

		tcp::resolver resolver(io_context);
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
		c->participant_map.clear();
		c->close();
		c->audio_ctx.stop_capture();
		c->audio_ctx.stop_playback();
		if (c->audio_ctx.capture_init) {
			c->audio_ctx.uninit_capture();
		}
		if (c->audio_ctx.playback_init) {
			c->audio_ctx.uninit_playback();
		}
		c->audio_ctx.uninit_vc_streams();
		work_guard.reset();
		io_context.stop();
		t.join();		
	}
	catch(std::exception& e){
		std::cerr << "Exception: " << e.what() << "\n";
	}
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}