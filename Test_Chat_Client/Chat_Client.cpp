

#include <glad/glad.h>
#include <GLFW/glfw3.h>

//#include <Windows.h>
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
static float dots = 0;
static float ping_timer = 0;
static float ping_interval = 3;
const std::string key = "basic_password_authorization:D";
std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();
std::chrono::duration<double> delta_time = std::chrono::duration<double>(0.0f);
const uint8_t max_participants = 16;
constexpr size_t packet_size = 512;
constexpr size_t packet_count = 64;
constexpr float sample_rate = 48000.0f;
constexpr ma_uint32 frame_size = 960;

class chat_client;
class SpectralSuppressor;
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
struct Audio_Context {
	Audio_Context(size_t frame_size, float sample_rate, chat_client* c_) : speaking{ false },
		current_gain{ 1.0f }, gain{ 20.0f }, noise_profile{ frame_size },
		silence_frames{ 0 }, hangover_duration{ 10 }, fading_out{ false },
		fade_index{ 0 }, fade_duration{ 30 }, rms_smoothed{ 0.0f },
		//spectral_suppressor{ frame_size },
		encoder{ nullptr }, decoder{ nullptr },
		hp_filter{},
		lp_filter{},
		c{c_}
	{
		ma_rb_init(packet_size * packet_count, nullptr, nullptr, &ring_buffer);
	}
	~Audio_Context() { std::cerr << "Audio Context destructor called\n"; }
	bool shutting_down = false;
	bool speaking;
	bool fading_out;
	float current_gain;
	float gain;
	NoiseProfile noise_profile;
	//SpectralSuppressor spectral_suppressor;
	HighPassFilter hp_filter;
	BiquadFilter lp_filter;
	ma_uint32 silence_frames;
	ma_uint32 fade_index;
	const ma_uint32 fade_duration;
	const ma_uint32 hangover_duration; // ~200ms at 48kHz
	float rms_smoothed;
	OpusEncoder* encoder;
	OpusDecoder* decoder;
	ma_rb ring_buffer;
	float input_float[frame_size]{};
	uint8_t packet[4096]{};
	chat_client* c;
};

void draw_disconnect_window(std::shared_ptr<chat_client>& c, GLFWwindow* window);
void draw_start_connection_window(std::shared_ptr<chat_client>& c, GLFWwindow* window);
template<typename Func>
void call_imgui(Func imgui_logic, GLFWwindow* window, std::shared_ptr<chat_client>& c);
int start_mic(ma_device& device);
int init_capture_device(ma_device& device_, ma_context& ma_context_, Audio_Context& ctx);
int init_playback_device(ma_device& device_, ma_context& ma_context_, Audio_Context& ctx);
int start_device(ma_device& device);
int stop_device(ma_device& device);
enum client_state {
	ready = 1,
	awaiting_connection = 2,
	connecting = 3,
	connected = 4,
	awaiting_authentication = 5,
	authenticated = 6,
	receiving_request = 7
};

enum participant_state {
	requesting_vc = 1,
	sending_vc_request = 2,
	vc_request_rejected = 3,
	in_vc = 4,
	neutral = 5
};

struct participant_client_data {
	participant_client_data(chat_participant p) : p{ p }, ps{participant_state::neutral} {}
	chat_participant p;
	participant_state ps;
};

class chat_client : public std::enable_shared_from_this<chat_client>
{
public:
	using client_ptr = std::shared_ptr<chat_client>;
	static client_ptr create(boost::asio::io_context& io_context,
		const tcp::resolver::results_type& endpoints, GLFWwindow* window) {
		return client_ptr(new chat_client(io_context, endpoints,
			window));
	}

	client_state state = client_state::awaiting_connection;
	std::vector<std::string>participant_names;
	std::vector<chat_participant>participants;
	std::unordered_set<uint8_t>vc_partner_ids;
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
	Audio_Context playback_ctx;
	Audio_Context capture_ctx;
	std::string session_token;//16 bytes/chars
	bool capture_init = false;
	bool playback_init = false;
	void start_capture() {
		running_capture_ = true;
		if (start_device(capture_device) == -1) {
			return;
		}
		auto self = shared_from_this();
		boost::asio::post(io_context_, [this, self] {
			check_and_send();
			});
	}
	void stop_capture() {
		running_capture_ = false;
		boost::system::error_code ec;
		stop_device(capture_device);
	}
	void start_playback() {
		running_playback_ = true;
		if (start_device(playback_device) == -1) {
			return;
		}
		auto self = shared_from_this();
		boost::asio::post(io_context_, [this,self] {
			check_and_read();
		});
	}
	void stop_playback() {
		running_playback_ = false;
		boost::system::error_code ec;
		stop_device(playback_device);
	}
	void uninit_capture() {
		ma_device_uninit(&capture_device);
		capture_init = false;
	}
	void uninit_playback() {
		ma_device_uninit(&playback_device);
		playback_init = false;
	}
	void init_capture(ma_device& capture_device_, ma_context& ma_capture_context_, Audio_Context& capture_ctx_) {
		init_capture_device(capture_device_, ma_capture_context_, capture_ctx_);
		capture_init = true;
	}
	void init_playback(ma_device& playback_device_, ma_context& ma_playback_context_, Audio_Context& playback_ctx_) {
		init_playback_device(playback_device_, ma_playback_context_, playback_ctx_);
		playback_init = true;
	}
	void check_close_socket() {
		if (!running_capture_ && !running_playback_) {
			boost::system::error_code ec;
			udp_socket->close();
		}
	}
	void try_reconnect() {
		if(!socket_->is_open()){
			socket_ = std::make_unique<tcp::socket>(io_context_);
			socket_->open(boost::asio::ip::tcp::v4()); // reuse same io_ctx			
		}
		state = client_state::connecting;
		do_connect(endpoints);
	}
	void send_ready_notification() {
		chat_message msg;
		std::string text = "ready";
		msg.body_length(text.length());
		msg.set_message_type(message_type::ready_notification);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		write(msg);
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
	}
private:
	void check_and_read() {
		if (!running_playback_) { return; }
		auto self = shared_from_this();
		udp_socket->async_receive_from(boost::asio::buffer(recv_buffer_), server_endpoint,
			[this, self](boost::system::error_code ec, std::size_t bytes) {
				if (!ec && bytes > 0) {
					void* pwrite = nullptr;
					size_t write_size = bytes;
					if (ma_rb_acquire_write(&playback_ctx.ring_buffer, &write_size, &pwrite) ==
						MA_SUCCESS && write_size >= bytes) {
						std::memcpy(pwrite, recv_buffer_.data(), bytes);
						ma_rb_commit_write(&playback_ctx.ring_buffer, bytes);
					}
				}
				check_and_read();
			});
	}
	void check_and_send(){
		if (!running_capture_) { return; }
		void* pread_void = nullptr;
		size_t read_size = sizeof(uint16_t);
		if (ma_rb_acquire_read(&capture_ctx.ring_buffer, &read_size, &pread_void) ==
			MA_SUCCESS && read_size >= sizeof(uint16_t)) {
			uint16_t len = 0;
			std::memcpy(&len, pread_void, sizeof(len));
			size_t total = sizeof(len) + len;
			if (read_size >= total) {
				auto buffer = boost::asio::buffer(pread_void, total);
				auto self = shared_from_this();
				udp_socket->async_send_to(buffer, server_endpoint,
					[this, self, total](boost::system::error_code ec, std::size_t bytes) {
						if (!ec && bytes == total) {
							ma_rb_commit_read(&capture_ctx.ring_buffer, total);
						}
						else {
							ma_rb_commit_read(&capture_ctx.ring_buffer, total);
						}
						check_and_send();
					});
				return;
			}
		}
		auto self = shared_from_this();
		boost::asio::steady_timer* timer = new boost::asio::steady_timer(io_context_);
		timer->expires_after(std::chrono::milliseconds(2));
		timer->async_wait([this, self, timer](boost::system::error_code) {
			delete timer;
			check_and_send();
		});
	}
	void add_participants(chat_message& m) {
		participants.clear();
		participant_map.clear();
		participant_client_map.clear();
		if (m.body_length() < sizeof(uint8_t)) { return; }
		uint8_t ps = 0;
		char* ptr = m.body();
		std::memcpy(&ps, ptr, sizeof(uint8_t));
		ptr += sizeof(uint8_t);
		char* end = m.body() + m.body_length();
		//std::cout << "ps = " << static_cast<int>(ps) << "\n";
		for (int i = 0; i < ps; i++) {
			if (ptr >= end) { break; }
			chat_participant p{};
			uint8_t len = 0;
			std::memcpy(&len, ptr, sizeof(uint8_t));
			ptr += sizeof(uint8_t);
			std::cout << "deserialize client\n";
			size_t size = p.deserialize(ptr);
			std::cout << "p.id1 = " << static_cast<int>(p.id) << "\n";

			ptr += len + 1;
			participant_client_data pcd{ p };
			std::cout << "pcd.id = " << static_cast<int>(pcd.p.id) << "\n";
			participants.push_back(p);
			participant_map.emplace(p.id, p);
			participant_client_map.emplace(p.id,pcd);
			std::cout << "pcm pcd.id = " << static_cast<int>(participant_client_map.at(p.id).p.id) << "\n";

		
		}

	}
	void send_authentication() {
		chat_message auth;
		auth.body_length(key.length());
		auth.set_message_type(message_type::authentication_response);
		std::memcpy(auth.body(), key.c_str(), auth.body_length());
		auth.encode_header();
		std::string auth_string = std::string(auth.body(), auth.body_length());
		write(auth);
	}
	std::string decode_session_token(chat_message& m) {
		if (m.body_length() < sizeof(uint8_t)) { return "bad"; }
		std::string token = std::string(m.body(), m.body_length());		
		return token;
	}
	void send_start_room_request() {
		chat_message msg;
		std::string text = "enter room";
		//uint8_t id = me.id;
		msg.body_length(text.length());
		//msg.body_length(sizeof(id));
		msg.set_message_type(message_type::start_room_request);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		//std::memcpy(msg.body(), &id, msg.body_length());
		msg.encode_header();
		write(msg);
	}
	void read_id(chat_message& m) {
		std::memcpy(&me.id, m.body(), sizeof(uint8_t));
	}
	void send_udp_port() {
		chat_message msg;
		std::cout << "sending udp port " << udp_port_client << "\n";
		std::string port = std::to_string(udp_port_client);
		msg.body_length(port.size());
		msg.set_message_type(message_type::send_udp_port);
		std::memcpy(msg.body(), port.data(), msg.body_length());
		msg.encode_header();
		std::cout << "send udp port info\n";
		write(msg);
	}
	void receive_vc_request(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), sizeof(uint8_t));
		auto it = participant_client_map.find(sender_id);
		if (it != participant_client_map.end()) {
			it->second.ps = participant_state::requesting_vc;
		}
		requests_.emplace(sender_id);
		state = client_state::receiving_request;
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
		std::cout << "vc_request_accepted() hex dump:\n";
		hex_dump(m.body(), m.body_length());
		uint8_t count = 0;
		vc_partner_ids.clear(); //only will get 2 vc participants currently
		std::memcpy(&count, m.body(), sizeof(count));
		if (count > max_participants) { return; }
		for (int i = 0; i < count; i++) {
			uint8_t temp_id = 0;
			std::memcpy(&temp_id, m.body() + sizeof(uint8_t) + sizeof(uint8_t) * i, sizeof(uint8_t));
			if (temp_id != me.id) {
				if (participant_map.find(temp_id) == participant_map.end()) { continue; }
				participant_client_map.at(temp_id).ps = participant_state::in_vc;//will throw error if not in map
				vc_partner_ids.emplace(temp_id);
			}
		}
		vc_room_id = 0;
		std::memcpy(&vc_room_id, m.body() + sizeof(uint8_t) + count, sizeof(vc_room_id));
		udp_port_server = 0;
		std::memcpy(&udp_port_server, m.body() + sizeof(uint8_t) + count + sizeof(vc_room_id), sizeof(udp_port_server));
		server_endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), udp_port_server);
		send_udp_port();
		/*auto iter = vc_partner_ids.begin();
		for (; iter != vc_partner_ids.end(); ++iter) {
			if (participant_map.find(*iter) == participant_map.end()) { continue; }
			participant_client_map.at(*iter).ps = participant_state::in_vc;//will throw error if not in map
		}*/
	}
	void start_vc(ma_device& device_) {
		udp_socket->open(udp::v4());
		start_mic(device_);
	}
	//can prob delete this
	void send_vc() {
		std::array<uint8_t, 512> voice_data{};//TODO get data
		udp_socket->async_send_to(boost::asio::buffer(voice_data),
			server_endpoint,
			[this](boost::system::error_code ec, std::size_t bytes_sent) {
				if (!ec) {
					udp_timer_.expires_after(std::chrono::milliseconds(20));
					udp_timer_.async_wait([this](boost::system::error_code) {
						send_vc(); });
				}
				else {
					//TODO end the voice chat
					std::cerr << "udp send error: " << ec.message() << "\n";
					udp_socket->close();
				}
			}
		);

	}

	void process_msg_type(chat_message& m) {
		//std::cout << "receive msg type " << static_cast<int>(m.msg_type) << "\n";
		switch (m.msg_type) {
			case message_type::chat:
				msg_history.push_back(std::string(read_msg_.body(), read_msg_.body_length()));
				msg_rcvd = true;
				break;
			case message_type::name_challenge:
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
				break;
			case message_type::particpants_request:
				std::cout << "\n";
				//hex_dump(m.body(), m.body_length());
				std::cout << "\n";
				add_participants(m);
				std::cout << "post add_participant my id = " << static_cast<int>(participant_client_map.at(me.id).p.id) << "\n";
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
				std::cout << "got id " << static_cast<int>(me.id) << "\n";
				break;
			case message_type::recieve_vc_request: 
				receive_vc_request(m);
				break;			
			case message_type::reject_vc_request:
				vc_request_rejected(m);
				break;
			case message_type::accept_vc_request:
				//vc_request_accepted(m);
				//voice_enabled = true;
				break;
			case message_type::start_vc: {
				std::cout << "start_vc\n";
				std::string token = decode_session_token(m);
				if (token == session_token) {
					start_capture();
					start_playback();
				}
				break;
			}
			case message_type::bad_message: {
				session_token = "bad";
				authenticated = false;
				state = client_state::awaiting_connection;
				break;
			}
			default:
				std::cout << "unknown message\n";
		}
		do_read_header();
	}
	void do_connect(const tcp::resolver::results_type& endpoints)
	{
		boost::asio::async_connect(*socket_, endpoints,
			[this](boost::system::error_code ec, tcp::endpoint) {
				//std::cout << "async_connect()\n";
				if (!ec) {
					//std::cout << "!ec\n";
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
						//std::cout << "delayed try reconnect - " << retry_delay << "\n";
						try_reconnect();
						});
					//std::cerr << "connect error: " << ec.message() << "\n";
				}
			});
	}
	void do_read_header() {
		//std::cout << "do_read_header()\n";
		boost::asio::async_read(*socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this](boost::system::error_code ec, std::size_t/*length*/) {
				if (!ec && read_msg_.decode_header()) {	
					if (read_msg_.msg_type == message_type::particpants_request) {
						//return;
					}
					do_read_body();
				}
				else {
					std::cout << "server disconnect header: " << ec.message() << "\n";
					std::cout << "data is[" << read_msg_.data() << "]\n";
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
	void do_read_body() {
		boost::asio::async_read(*socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this](boost::system::error_code ec, std::size_t /*length*/)
			{
				if (!ec) {
					std::string header = std::string(read_msg_.data(), chat_message::header_length);
					std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					std::cout << "read msg header[" << header << "] body [" << body << "]\n";
					process_msg_type(read_msg_);
				}
				else {
					std::cout << "server disconnect body: " << ec.message() << "\n";
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
				std::cout << "write msg header[" << header << "] body [" << body << "]\n";

				if (!ec) {
					if (write_msgs_.front().msg_type == message_type::name_change_request) {						
						//return;
					}
					if (write_msgs_.front().msg_type == message_type::send_udp_port) {
						std::cout << "client sending udp port info\n";
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
					std::cout << "server disconnect do_write: " << ec.message() << "\n";
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
	void add_message(const std::string& msg) {
		msgs.push_back(msg);
		scroll_to_bottom = true;
	}
private:
	GLFWwindow* window;
	boost::asio::io_context& io_context_;
	std::shared_ptr<tcp::socket> socket_;
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
	std::atomic<bool> running_capture_{ false };
	std::atomic<bool> running_playback_{ false };
	ma_device capture_device;
	ma_device playback_device;
	ma_context ma_capture_context;
	ma_context ma_playback_context;
	std::array<uint8_t, 1500>recv_buffer_;
	chat_client(boost::asio::io_context& io_context,
		const tcp::resolver::results_type& endpoints, GLFWwindow* window)
		: io_context_(io_context), socket_(std::make_shared<tcp::socket>(io_context)),
		udp_socket(std::make_shared<udp::socket>(io_context, udp::endpoint(udp::v4(), 0))), window(window), endpoints(endpoints),
		timer_(std::make_unique<boost::asio::steady_timer>(io_context)),
		udp_timer_{ udp_socket->get_executor() }, capture_ctx{ Audio_Context(frame_size, sample_rate, this) }, playback_ctx{ Audio_Context(frame_size, sample_rate, this) }, me()
	{
		me.id = 0;
		init_capture(capture_device, ma_capture_context, capture_ctx);
		init_playback(playback_device, ma_playback_context, playback_ctx);
		do_connect(endpoints);
		session_token = "";
		auto client_ep = udp_socket->local_endpoint();
		udp_port_client = client_ep.port();
		std::cout << "client udp_port = " << udp_port_client << "\n";
	}

};

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
				c->write(msg);
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

void draw_chat_window(std::shared_ptr<chat_client>& c, GLFWwindow* window) {
	static bool no_resize = true;
	static bool no_move = true;
	static bool no_titlebar = true;
	static bool no_menu = false;
	static bool no_inputs = change_name;

	ImGuiWindowFlags window_flags = 0;
	if (no_resize)          window_flags |= ImGuiWindowFlags_NoResize;
	if (no_move)            window_flags |= ImGuiWindowFlags_NoMove;
	if (no_titlebar)        window_flags |= ImGuiWindowFlags_NoTitleBar;
	if (!no_menu)           window_flags |= ImGuiWindowFlags_MenuBar;
	const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
	//std::cout << "c.state = " << c.state << "\n";
	if (c->state == client_state::awaiting_connection || c->state == client_state::connecting || !authenticated) {		
		draw_disconnect_window(c, window);
		return;
	}
	if (c->state == client_state::ready || c->state == client_state::awaiting_authentication) {
		draw_start_connection_window(c, window);
		return;
	}
	if (ImGui::IsMouseClicked(0)) {
		first_enter = false;
	}
	//std::cout << "change_name = " << change_name << "awaiting_change_response = " << awaiting_change_response << "\n";
	if (change_name && !awaiting_change_response) {
		//std::cout << "change name\n";
		ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 0, main_viewport->WorkPos.y + 0), ImGuiCond_Once);
		ImGui::SetNextWindowSize(ImVec2(main_viewport->Size.x, main_viewport->Size.y), ImGuiCond_Once);
		ImGui::Begin("Name", NULL, window_flags);
		draw_menu_bar(c, window);
		ImGui::SetCursorPos(ImVec2(main_viewport->Size.x * 0.5f - 100.0f, main_viewport->Size.y * 0.5f));
		ImGui::SetNextItemWidth(200);
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
			c->write(msg);
			hint = "";
			awaiting_change_response = true;
			text.clear();
		}

		ImGui::End();
		return;
	}
	if(awaiting_change_response)//render the windows but don't allow interaction until approval response
		window_flags |= ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 0, main_viewport->WorkPos.y + 0), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(main_viewport->Size.x, main_viewport->Size.y), ImGuiCond_Once);
	ImGui::Begin("Input", NULL, window_flags);
	ImGui::SetCursorPos(ImVec2(10, 30));
	draw_menu_bar(c, window);
	//ImGui::SetCursorPos(ImVec2(10, main_viewport->Size.y - 90.0f));
	ImGui::SetCursorPos(ImVec2(10, 650.0f));
	ImGui::SetNextItemWidth(ImGui::GetWindowSize().x - 200);
	std::string text;
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0) && first_enter)
	{
		ImGui::SetKeyboardFocusHere(0);
	}
	if (ImGui::InputText("##Input Text", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
		chat_message msg;
		msg.body_length(text.length());
		msg.set_message_type(message_type::chat);
		std::memcpy(msg.body(), text.c_str(), msg.body_length());
		msg.encode_header();
		//std::cout << "sending msg: " << msg.data() << "\n";
		c->write(msg);
		text.clear();
		was_focused = true;
	}
	if (was_focused) {
		ImGui::SetKeyboardFocusHere(-1);
		was_focused = false;
	}
	ImGui::SetCursorPos(ImVec2(10, 40));
	ImGui::BeginChild("Output Text", ImVec2(ImGui::GetWindowSize().x - 200, 600.0f), ImGuiChildFlags_Borders);
	for (const auto& m : msg_history) {
		//ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 200);
		//ImGui::TextUnformatted("%s", m.c_str());
		//ImGui::PopTextWrapPos();
		ImGui::TextWrapped("%s", m.c_str());
	}
	if (msg_rcvd) {
		ImGui::SetScrollHereY(0.999f);
		msg_rcvd = false;
	}
	ImGui::EndChild();
	ImGui::SetCursorPos(ImVec2(ImGui::GetWindowSize().x - 170.0f, 40.0f));

	ImGuiWindowFlags scroll_flags = 0;
	scroll_flags |= ImGuiWindowFlags_NoTitleBar;
	scroll_flags |= ImGuiWindowFlags_NoMove;
	scroll_flags |= ImGuiWindowFlags_NoResize;
	scroll_flags |= ImGuiWindowFlags_NoCollapse;
	scroll_flags |= ImGuiWindowFlags_HorizontalScrollbar;

	ImGui::BeginChild("ChildL", ImVec2(150.0f, 600), ImGuiChildFlags_None, scroll_flags);
	ImGui::Text("Room");
	ImGui::Separator();	
	for (int i = 0; i < c->participant_names.size(); i++) {
		ImGui::Text(c->participant_names[i].c_str());
	}
	/*for (int i = 0; i < c.participants.size(); i++) {
		//ImGui::Text(c.participant_names[i].c_str());
		std::string header_label = std::string(c.participants[i].name) + "##participant_" + std::to_string(i);
		if (ImGui::CollapsingHeader(header_label.c_str())) {
			ImGui::Indent();
			voice_chat_state vc_state = c.participants[i].vc_state;
			//bool recieving_request = c.participants[i].recieving_request;
			const char* label = "Voice";
			if (ImGui::CollapsingHeader(label)) {
				if (vc_state != voice_chat_state::recieving_request) {
					if (ImGui::Button("Send request")) {
						//send_vc_request
						chat_message msg;
						uint8_t sender_id = c.me.id;
						uint8_t receiver_id = c.participants[i].id;
						msg.body_length(sizeof(sender_id) + sizeof(receiver_id));
						msg.set_message_type(message_type::send_vc_request);
						std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
						std::memcpy(msg.body() + sizeof(sender_id), &receiver_id, sizeof(receiver_id));
						msg.encode_header();
						c.write(msg);//TODO
					}
				}
				if (vc_state == voice_chat_state::recieving_request) {
					ImGui::Text("Accept request?");
					ImGui::Button("Yes");
					ImGui::SameLine();
					ImGui::Button("No");
				}
			}
			ImGui::Unindent();
		}
	}*/
	/*std::unordered_map<uint8_t, chat_participant>::iterator iter = c.participant_map.begin();
	for (; iter != c.participant_map.end(); ++iter) {
		//ImGui::Text(c.participant_names[i].c_str());
		std::string header_label = std::string(iter->second.name) + "##participant_" + std::to_string(iter->second.id);
		if (ImGui::CollapsingHeader(header_label.c_str())) {
			ImGui::Indent();
			voice_chat_state vc_state = iter->second.vc_state;
			//bool recieving_request = c.participants[i].recieving_request;
			const char* label = "Voice";
			if (ImGui::CollapsingHeader(label)) {
				if (vc_state != voice_chat_state::recieving_request) {
					if (ImGui::Button("Send request")) {
						//send_vc_request
						chat_message msg;
						uint8_t sender_id = c.me.id;
						uint8_t receiver_id = iter->second.id;
						msg.body_length(sizeof(sender_id) + sizeof(receiver_id));
						msg.set_message_type(message_type::send_vc_request);
						std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
						std::memcpy(msg.body() + sizeof(sender_id), &receiver_id, sizeof(receiver_id));
						msg.encode_header();
						c.write(msg);//TODO
					}
				}
				if (c.state == client_state::receiving_request) {
					if (c.requests_.count(iter->second.id)) {
						ImGui::Text("Accept request?");
						ImGui::Button("Yes");
						ImGui::SameLine();
						ImGui::Button("No");
					}
				}
			}
			ImGui::Unindent();
		}
	}*/
	std::unordered_map<uint8_t, participant_client_data>::iterator iter = c->participant_client_map.begin();
	for (; iter != c->participant_client_map.end(); ++iter) {
		//ImGui::Text(c.participant_names[i].c_str());
		std::string header_label = std::string(iter->second.p.name) + "##participant_" + std::to_string(iter->second.p.id);
		if (ImGui::CollapsingHeader(header_label.c_str())) {
			ImGui::Indent();
			voice_chat_state vc_state = iter->second.p.vc_state;
			//bool recieving_request = c.participants[i].recieving_request;
			const char* label = "Voice";
			if (ImGui::CollapsingHeader(label)) {
				//if (!iter->second.sending_vc_request) {
					ImGui::BeginDisabled(iter->second.ps == participant_state::sending_vc_request ||
						iter->second.ps == participant_state::requesting_vc || iter->second.ps == participant_state::in_vc
					|| c->state == participant_state::in_vc);
					if (ImGui::Button("Send request")) {
						std::cout << "sending vc request\n";
						//send_vc_request
						chat_message msg;
						uint8_t sender_id = c->me.id;
						uint8_t receiver_id = iter->second.p.id;
						std::cout << "Send request sender_id[" << static_cast<int>(sender_id) << "] receiver_id[" << static_cast<int>(receiver_id) << "]\n";
						msg.body_length(sizeof(sender_id) + sizeof(receiver_id));
						msg.set_message_type(message_type::send_vc_request);
						std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
						std::memcpy(msg.body() + sizeof(sender_id), &receiver_id, sizeof(receiver_id));
						msg.encode_header();
						c->write(msg);//TODO
						iter->second.ps = participant_state::sending_vc_request;
					}
					ImGui::EndDisabled();
				//}
				if (iter->second.ps == participant_state::requesting_vc) {
						ImGui::Text("Accept request?");
						if (ImGui::Button("Yes")) {
							chat_message msg;
							uint8_t sender_id = iter->second.p.id;//i don't have sender and receiver ids for some reason
							uint8_t receiver_id = c->me.id;
							std::cout << "Accept request? sender_id[" << static_cast<int>(sender_id) << "] receiver_id[" << static_cast<int>(receiver_id) << "]\n";
							msg.body_length(sizeof(sender_id) + sizeof(receiver_id));
							msg.set_message_type(message_type::accept_vc_request);
							std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
							std::memcpy(msg.body() + sizeof(sender_id), &receiver_id, sizeof(receiver_id));
							msg.encode_header();
							c->write(msg);
							iter->second.ps = participant_state::neutral;
						}
						ImGui::SameLine();
						if (ImGui::Button("No")) {
							chat_message msg;
							uint8_t sender_id = iter->second.p.id;
							uint8_t receiver_id = c->me.id;
							msg.body_length(sizeof(sender_id) + sizeof(receiver_id));
							msg.set_message_type(message_type::reject_vc_request);
							std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
							std::memcpy(msg.body() + sizeof(sender_id), &receiver_id, sizeof(receiver_id));
							msg.encode_header();
							c->write(msg);//TODO
							iter->second.ps = participant_state::neutral;
						}					
				}
			}
			ImGui::Unindent();
		}
	}
	ImGui::EndChild();
	ImGui::End();
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
		c->write(msg);
		text.clear();
	}
	ImGui::End();
}
void draw_start_connection_window(std::shared_ptr<chat_client>& c, GLFWwindow* window) {
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
	ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 0, main_viewport->WorkPos.y + 0), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(main_viewport->Size.x, main_viewport->Size.y), ImGuiCond_Once);
	ImGui::Begin("Open", NULL, window_flags);
	draw_menu_bar(c, window);
	ImGui::SetCursorPos(ImVec2(main_viewport->Size.x * 0.5f - 100.0f, main_viewport->Size.y * 0.5f));
	ImGui::SetNextItemWidth(200);
	std::string text = "Start connection";
	for (int i = 0; i < dots; i++) {
		text = text + ".";
	}
	dots = std::fmod(dots + static_cast<float>(delta_time.count()), 4);
	ImGui::Text(text.c_str());
	ImGui::End();
	if (c->state == client_state::ready) {
		c->send_ready_notification();
	}
}
void draw_disconnect_window(std::shared_ptr<chat_client>& c, GLFWwindow* window) {
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
	ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + 0, main_viewport->WorkPos.y + 0), ImGuiCond_Once);
	ImGui::SetNextWindowSize(ImVec2(main_viewport->Size.x, main_viewport->Size.y), ImGuiCond_Once);
	ImGui::Begin("Connecting", NULL, window_flags);
	draw_menu_bar(c, window);
	ImGui::SetCursorPos(ImVec2(main_viewport->Size.x * 0.5f - 100.0f, main_viewport->Size.y * 0.5f));
	ImGui::SetNextItemWidth(200);
	std::string text = "Connecting";
	for (int i = 0; i < dots; i++) {
		text = text + ".";
	}
	dots = std::fmod(dots + static_cast<float>(delta_time.count()),4);
	ImGui::Text(text.c_str());
	ImGui::End();
	//ping_timer += static_cast<float>(delta_time.count());
	//if (ping_timer > ping_interval) {
		/*chat_message msg;
		std::string textt = "ping";
		msg.body_length(textt.length());
		msg.set_message_type(message_type::ping);
		std::memcpy(msg.body(), textt.c_str(), msg.body_length());
		msg.encode_header();
		c.write(msg);*/
		//ping_timer = 0.0f;
	if(c->state == client_state::awaiting_connection){
		//std::cout << "awaiting_connection try_reconnect()\n";
		c->try_reconnect();
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
void duplex_data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
	auto* ctx = static_cast<Audio_Context*>(device->pUserData);
	auto* in = static_cast<const int16_t*>(input);
	auto* out = static_cast<int16_t*>(output);

	float new_rms = compute_rms(in, frame_count);
	ctx->rms_smoothed = 0.2f * ctx->rms_smoothed + 0.8f * new_rms;
	//ctx->rms_smoothed = new_rms;
	if (ctx->rms_smoothed > upper_threshold) {
		ctx->silence_frames = 0;
		ctx->fade_index = ctx->fade_duration;
		ctx->fading_out = false;
		ctx->speaking = true; 
	}
	else if (ctx->rms_smoothed < lower_threshold) {
		ctx->silence_frames++;
		if (ctx->silence_frames > 10) {
			ctx->speaking = false;
			if (ctx->fade_index > 0) {
				ctx->fading_out = true;
				ctx->fade_index--;
			}
			else {
				ctx->fading_out = false;
			}
		}
	}

	float target_gain = ctx->speaking ? ctx->gain : 1.0f;
	ctx->current_gain += (target_gain - ctx->current_gain) * 0.05f;

	if (!ctx->speaking) {
		ctx->noise_profile.update(in, frame_count);
		if (!ctx->fading_out) {
			std::memset(out, 0, frame_count * sizeof(int16_t));
			return;
		}
	}
	for (ma_uint32 i = 0; i < frame_count; ++i) {
		float sample = static_cast<float>(in[i]);
		//float cleaned = sample - ctx->noise_profile.get(i);
		float cleaned = sample;
		if (std::abs(cleaned) < 10.0f) {
			cleaned = 0.0f;
		}
		cleaned *= ctx->current_gain;
		if (std::abs(cleaned) < 10.0f) {
			cleaned = 0.0f;
		}
		cleaned = ctx->hp_filter.process(cleaned);
		cleaned = ctx->lp_filter.process(cleaned);
		if (ctx->fading_out) {		
			float fade = static_cast<float>(ctx->fade_index) / ctx->fade_duration;
			cleaned *= fade;
		}
		out[i] = static_cast<int16_t>(std::clamp(cleaned, -32768.0f, 32767.0f));
	}	
	//assume my ring_buffer is initialized to hold 32KB
	uint8_t encoded[4000];
	float input_float[960];
	for (ma_uint32 i = 0; i < frame_count; ++i)
		input_float[i] = static_cast<float>(out[i]) / 32768.0f;

	ma_result result;
	int encoded_bytes = opus_encode_float(ctx->encoder, input_float, frame_count, encoded, sizeof(encoded));
	uint16_t len = static_cast<uint16_t>(encoded_bytes);
	const size_t header_size = sizeof(len);
	size_t total = header_size + size_t(encoded_bytes);

	void* pwrite_void = nullptr;
	size_t write_size = total;
	if (encoded_bytes <= 0) {
		//dropped frames. opus error
		return;
	}
	result = ma_rb_acquire_write(&ctx->ring_buffer, &write_size, &pwrite_void);
	if (result != MA_SUCCESS || write_size < total) {
		//dropped frames. call ma_rb_release()?
		return;
	}
	uint8_t* pwrite = static_cast<uint8_t*>(pwrite_void);

	std::memcpy(pwrite, &len, header_size);
	std::memcpy(pwrite + header_size, encoded, size_t(encoded_bytes));

	result = ma_rb_commit_write(&ctx->ring_buffer, total);

	//TODO don't decode in callback anymore?
	float decoded_buffer[(int)frame_size];
	int decoded_frames = opus_decode_float(ctx->decoder, encoded, encoded_bytes, decoded_buffer, frame_count, 0);
	if (decoded_frames < 0) { 
		memset(out, 0, frame_count * sizeof(uint16_t));
		return; }

	for (int i = 0; i < decoded_frames; ++i) {
		//out[i] = std::clamp(decoded_buffer[i], -1.0f, 1.0f);
		float amplified = decoded_buffer[i] * 1.5f;
		out[i] = static_cast<int16_t>(std::clamp(amplified * 32768.0f, -32768.0f, 32767.0f));
	}

	//memcpy(out, decoded_buffer, decoded_frames * sizeof(float));
}
void playback_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
	auto* ctx = static_cast<Audio_Context*>(device->pUserData);
	if (ctx->c->me.session_token.length() != 16) { std::cerr << "bad token\n"; return; }
	auto* out = static_cast<uint16_t*>(output);
	void* pread_void = nullptr;
	size_t read_size = sizeof(ctx->packet);
	ma_result result = ma_rb_acquire_read(&ctx->ring_buffer, &read_size, &pread_void);
	if (result != MA_SUCCESS || read_size < sizeof(uint16_t)) {
		return;
	}
	uint8_t* pread = static_cast<uint8_t*>(pread_void);
	uint16_t len = 0;
	std::memcpy(&len, pread, sizeof(len));
	
	size_t total = sizeof(len) + len;
	if (read_size < total) {
		return;
	}
	std::memcpy(ctx->packet, pread, total);
	ma_rb_commit_read(&ctx->ring_buffer, total);
	const uint8_t* encoded = ctx->packet + sizeof(len);
	int encoded_bytes = len;

	float decoded_buffer[(int)frame_size];
	int decoded_frames = opus_decode_float(ctx->decoder, encoded, encoded_bytes, decoded_buffer, frame_count, 0);
	if (decoded_frames < 0) {
		memset(out, 0, frame_count * sizeof(uint16_t));
		return;
	}
	for (int i = 0; i < decoded_frames; ++i) {
		//out[i] = std::clamp(decoded_buffer[i], -1.0f, 1.0f);
		float amplified = decoded_buffer[i] * 1.5f;
		out[i] = static_cast<int16_t>(std::clamp(amplified * 32768.0f, -32768.0f, 32767.0f));
	}
}
void capture_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
	auto* ctx = static_cast<Audio_Context*>(device->pUserData);
	auto* in = static_cast<const int16_t*>(input);
	if (ctx->c->me.session_token.length() != 16) { std::cerr << "bad token\n"; return; }
	int16_t cleaned_samples[frame_size];
	float new_rms = compute_rms(in, frame_count);
	ctx->rms_smoothed = 0.2f * ctx->rms_smoothed + 0.8f * new_rms;
	//ctx->rms_smoothed = new_rms;
	if (ctx->rms_smoothed > upper_threshold) {
		ctx->silence_frames = 0;
		ctx->fade_index = ctx->fade_duration;
		ctx->fading_out = false;
		ctx->speaking = true;
	}
	else if (ctx->rms_smoothed < lower_threshold) {
		ctx->silence_frames++;
		if (ctx->silence_frames > 10) {
			ctx->speaking = false;
			if (ctx->fade_index > 0) {
				ctx->fading_out = true;
				ctx->fade_index--;
			}
			else {
				ctx->fading_out = false;
			}
		}
	}

	float target_gain = ctx->speaking ? ctx->gain : 1.0f;
	ctx->current_gain += (target_gain - ctx->current_gain) * 0.05f;

	if (!ctx->speaking) {
		ctx->noise_profile.update(in, frame_count);
		if (!ctx->fading_out) {
			//std::memset(out, 0, frame_count * sizeof(int16_t));
			return;
		}
	}
	for (ma_uint32 i = 0; i < frame_count; ++i) {
		float sample = static_cast<float>(in[i]);
		//float cleaned = sample - ctx->noise_profile.get(i);
		float cleaned = sample;
		if (std::abs(cleaned) < 10.0f) {
			cleaned = 0.0f;
		}
		cleaned *= ctx->current_gain;
		if (std::abs(cleaned) < 10.0f) {
			cleaned = 0.0f;
		}
		cleaned = ctx->hp_filter.process(cleaned);
		cleaned = ctx->lp_filter.process(cleaned);
		if (ctx->fading_out) {
			float fade = static_cast<float>(ctx->fade_index) / ctx->fade_duration;
			cleaned *= fade;
		}
		cleaned_samples [i] = static_cast<int16_t>(std::clamp(cleaned, -32768.0f, 32767.0f));
	}
	//assume my ring_buffer is initialized to hold 32KB
	uint8_t encoded[4000];
	//float input_float[frame_size];
	for (size_t i = 0; i < frame_count; ++i)
		ctx->input_float[i] = static_cast<float>(cleaned_samples[i]) / 32768.0f;

	ma_result result;
	int encoded_bytes = opus_encode_float(ctx->encoder, ctx->input_float, frame_count, encoded, sizeof(encoded));
	if (encoded_bytes <= 0) {
		//dropped frames. opus error
		return;
	}
	voice_chat_message vm{};
	vm.set_header(ctx->c->me.session_token, encoded_bytes, ctx->c->vc_room_id, ctx->c->me.id);
	uint16_t len = static_cast<uint16_t>(encoded_bytes);
	const size_t header_size = sizeof(vm.header_length);
	size_t total = header_size + size_t(encoded_bytes);

	void* pwrite_void = nullptr;
	size_t write_size = total;
	result = ma_rb_acquire_write(&ctx->ring_buffer, &write_size, &pwrite_void);
	if (result != MA_SUCCESS || write_size < total) {
		//dropped frames. call ma_rb_release()?
		return;
	}
	uint8_t* pwrite = static_cast<uint8_t*>(pwrite_void);

	std::memcpy(pwrite, vm.get_data(), vm.header_length);
	std::memcpy(pwrite + vm.header_length, encoded, size_t(encoded_bytes));

	result = ma_rb_commit_write(&ctx->ring_buffer, total);
}
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

int init_capture_device(ma_device& device_, ma_context& ma_context_, Audio_Context& ctx) {
	//ctx = Audio_Context(frame_size, sample_rate);
	ctx.lp_filter.setLowPass(sample_rate, 6000.0f);
	ctx.hp_filter.set(sample_rate, 100.0f);
	int error;
	ctx.encoder = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_VOIP, &error);
	ctx.decoder = opus_decoder_create(sample_rate, 1, &error);
	if (error != OPUS_OK) {
		std::cerr << "opus init fail: " << opus_strerror(error) << "\n";
	}
	ma_context_init(NULL, 0, NULL, &ma_context_);

	ma_device_info* capture_devices;
	ma_uint32 capture_count;
	ma_context_get_devices(&ma_context_, NULL, NULL, &capture_devices, &capture_count);

	for (ma_uint32 i = 0; i < capture_count; i++) {
		std::cout << i << ": " << capture_devices[i].name << "\n";
	}

	ma_device_config config = ma_device_config_init(ma_device_type_duplex);
	config.capture.pDeviceID = &capture_devices[2].id;
	config.sampleRate = SAMPLE_RATE;
	config.playback.format = ma_format_s16;
	config.playback.channels = CHANNELS;
	config.capture.format = ma_format_s16;
	config.capture.channels = CHANNELS;
	config.dataCallback = capture_callback;
	config.notificationCallback = on_notification;
	config.periodSizeInFrames = frame_size;
	config.sampleRate = sample_rate;
	config.pUserData = &ctx;

	size_t buffer_size_bytes = packet_size * packet_count;
	ma_result result = ma_rb_init(frame_size, nullptr, nullptr, &ctx.ring_buffer);

	if (ma_device_init(&ma_context_, &config, &device_) != MA_SUCCESS) {
		std::cerr << "Failed to initialize audio device\n";
		return -1;
	}
}
int init_playback_device(ma_device& device_, ma_context& ma_context_, Audio_Context& ctx) {
	//ctx = Audio_Context(frame_size, sample_rate);
	int error;
	ctx.decoder = opus_decoder_create(sample_rate, 1, &error);
	if (error != OPUS_OK) {
		std::cerr << "opus init fail: " << opus_strerror(error) << "\n";
	}
	ma_context_init(NULL, 0, NULL, &ma_context_);

	ma_device_info* playback_devices;
	ma_uint32 playback_count;
	ma_context_get_devices(&ma_context_, &playback_devices, &playback_count, NULL, NULL);

	for (ma_uint32 i = 0; i < playback_count; i++) {
		std::cout << i << ": " << playback_devices[i].name << "\n";
	}

	ma_device_config config = ma_device_config_init(ma_device_type_duplex);
	config.playback.pDeviceID = &playback_devices[3].id;
	config.sampleRate = SAMPLE_RATE;
	config.playback.format = ma_format_s16;
	config.playback.channels = CHANNELS;
	config.dataCallback = playback_callback;
	config.notificationCallback = on_notification;
	config.periodSizeInFrames = frame_size;
	config.sampleRate = sample_rate;
	config.pUserData = &ctx;

	size_t buffer_size_bytes = packet_size * packet_count;
	ma_result result = ma_rb_init(frame_size, nullptr, nullptr, &ctx.ring_buffer);

	if (ma_device_init(&ma_context_, &config, &device_) != MA_SUCCESS) {
		std::cerr << "Failed to initialize audio device\n";
		return -1;
	}
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
int main(int argc, char* argv[])
{	 
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN8
	std::cout << "WinAPI version OK\n";
#else
	std::cout << "WinAPI version too low\n";
#endif

#if BOOST_WINAPI_PARTITION_APP
	std::cout << "Partition: APP\n";
#endif
#if BOOST_WINAPI_PARTITION_SYSTEM
	std::cout << "Partition: SYSTEM\n";
#endif

	//mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	//screenWidth = mode->width;
	//screenHeight = mode->height;
	msg_history.reserve(100);
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return 1;
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	GLFWwindow* window = glfwCreateWindow((int)(1280 * main_scale), (int)(800 * main_scale), "Chat", nullptr, nullptr);
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

	std::thread test_thread(worker_thread);
	std::this_thread::sleep_for(std::chrono::seconds(1));
	running = false;
	test_thread.join();

	try {
		/*if (argc != 3) {
			std::cerr << "Usage: chat_client <host> <port> \n";
			return 1;
		}*/
		boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13);
		boost::asio::io_context io_context;

		std::string ip = "159.89.49.248";
		std::string port = "5000";
		tcp::resolver resolver(io_context);
		//auto endpoints = resolver.resolve(argv[1], argv[2]);
		auto endpoints = resolver.resolve(ip, port);
		std::shared_ptr<chat_client> c = chat_client::create(io_context, endpoints, window);
		c->server_ip = ip;

		std::thread t([&io_context]() { io_context.run(); });
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
		c->msgs.clear();
		c->participant_names.clear();
		c->participants.clear();
		c->participant_map.clear();
		c->close();
		c->stop_capture();
		c->stop_playback();
		if (c->capture_init) {
			c->uninit_capture();
		}
		if (c->playback_init) {
			c->uninit_playback();
		}
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