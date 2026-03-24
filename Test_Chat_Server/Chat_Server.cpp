//test build
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <queue>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <utility>
#include <functional>
#include <boost/asio.hpp>
#include <boost/chrono.hpp>
#include <boost/thread/thread.hpp>
#include <boost/system.hpp>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <string>
#include <array>
#include "Chat_Message.h"
#include "Chat_Participant.h"
#include "Voice_Chat_Participant.h"
#include "Voice_Chat_Message.h"
//#include "kiss_fft.h"
#include "opus.h"
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <ssl/include/openssl/ssl.h>
#include <ssl/include/openssl/err.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#define BOOST_NETWORK_ENABLE_HTTPS
#include <boost/asio/ssl.hpp>

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
typedef std::deque<chat_message> chat_message_queue;
typedef std::deque<voice_chat_message> vc_message_queue;
const std::string key = "basic_password_authorization:D";
const uint8_t max_participants = 16;
const uint8_t max_name_length = 16;
typedef std::shared_ptr<chat_participant> chat_participant_ptr;
typedef std::shared_ptr<voice_chat_participant> vc_participant_ptr;
std::queue<uint8_t> free_ids{};
uint8_t next_id = 1;
constexpr size_t packet_size = 512;
constexpr size_t packet_count = 64;

constexpr uint16_t udp_port = 5000;

struct packet {
	std::unique_ptr<uint8_t[]> data;
	size_t size;
};
typedef std::deque<packet> packet_queue;

uint8_t generate_id() {
	if (!free_ids.empty()) {
		uint8_t id = free_ids.front();
		free_ids.pop();
		std::cout << "returning free_id " << static_cast<int>(id) << "\n";
		return id;
	}
	if (next_id >= 254) {
		std::cerr << "ERROR max participants reached\n";
		return 255;//reserve id 255 for overflow error
	}
	else {
		uint8_t id = next_id++;
		std::cout << "returning new id " << static_cast<int>(id) << "\n";
		return id;
	}
}
void release_id(uint8_t id) {
	std::cout << "release id " << static_cast<int>(id) << "\n";
	free_ids.push(id);
}
class id_generator {
public:
	std::queue<uint8_t> free_ids{};
	uint8_t next_id = 1;
	id_generator() {}
	uint8_t generate_id() {
		if (!free_ids.empty()) {
			uint8_t id = free_ids.front();
			free_ids.pop();
			std::cout << "returning free_id " << static_cast<int>(id) << "\n";
			return id;
		}
		if (next_id >= 254) {
			std::cerr << "ERROR max participants reached\n";
			return 255;//reserve id 255 for overflow error
		}
		else {
			uint8_t id = next_id++;
			std::cout << "returning new id " << static_cast<int>(id) << "\n";
			return id;
		}
	}
	void release_id(uint8_t id) {
		std::cout << "releasing id " << static_cast<int>(id) << "\n";
		free_ids.push(id);
	}
};

SSL_CTX* create_context()
{
	const SSL_METHOD* method;
	SSL_CTX* ctx;

	method = TLS_server_method();

	ctx = SSL_CTX_new(method);
	if (!ctx) {
		perror("Unable to create SSL context");
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}

	return ctx;
}

void configure_context(SSL_CTX* ctx)
{
	/* Set the key and cert */
	if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}

	if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}
}

class voice_chat_room;
class voice_chat_room : public std::enable_shared_from_this<voice_chat_room> {
public:
	voice_chat_room(std::shared_ptr<udp::socket> udp_socket, udp::endpoint& udp_endpoint,
		boost::asio::io_context& io_context, uint8_t room_id)
		:
		udp_socket_(udp_socket), udp_endpoint_(udp_endpoint),
		io_context_(io_context), timer_{ boost::asio::steady_timer(io_context_, boost::asio::chrono::milliseconds(1))},
		room_id_(room_id)
	{}
	void join_room(chat_participant_ptr participant_)
	{
		add_participant(participant_);
		participant_->set_vc_room_id(room_id_);
		participant_->set_vc_room_id(room_id_);
		participant_->start_read_vc_rb();
		participant_->start_read_vc_rb();
	}
	void leave_room(chat_participant_ptr participant_) {
		remove_participant(participant_);
		participant_->set_vc_room_id(0);
		participant_->stop_read_vc_rb();
	}
	void get_participant_ids(uint8_t* ids, uint8_t& count) {
		count = 0;
		auto iter = participants_.begin();
		uint8_t id_size = sizeof(chat_participant::id);
		for (; iter != participants_.end() && count < max_participants; ++iter) {
			uint8_t id = iter->second->id;
			std::memcpy(ids + count * id_size, &id, id_size);
			++count;
		}
	}
	void add_participant(chat_participant_ptr participant_) {
		std::cout << "try add vc participant\n";
		if (participants_.find(participant_->id) != participants_.end() || participants_.size() > max_participants || participant_->get_vc_room_id()) { 
			std::cout << "fail add vc participant with room id " << static_cast<int>(participant_->get_vc_room_id()) << " to vc room " << static_cast<int>(room_id_) << "\n";

			return; }
		std::cout << "add participant " << static_cast<int>(participant_->id) << " to vc room " << static_cast<int>(room_id_) << "\n";
		participants_.emplace(participant_->id, participant_);
	}
	void remove_participant(chat_participant_ptr participant_) {
		participants_.erase(participant_->id);
		std::cout << "erase participant_ " << participant_->name << " " << static_cast<int>(participant_->id) << " " << " from vc_room " << static_cast<int>(room_id_) << "\n";
	}
	void route_vc_msg_to_sender(std::shared_ptr<voice_chat_message> recv_vc_msg_) {
		//std::cout << "routing msg to sender_id[" << static_cast<int>(recv_vc_msg_->sender_id) << "]\n";
		//TODO verify participant not removed. this seg faults when they leave currently

		if (participants_[recv_vc_msg_->sender_id]->get_vc_room_id() == 0) { 	
			//invalid sender. tcp server handles clean up
			std::cout << "vc room is 0\n";
			return; 
		}
		participants_[recv_vc_msg_->sender_id]->write_vc_msg_to_rb(recv_vc_msg_);
	}
	void send_message_to_playback(std::shared_ptr<voice_chat_message> recv_vc_msg_, uint8_t& sender_id) {
		auto self = shared_from_this();
		auto iter = participants_.begin();
		for (; iter != participants_.end(); ++iter) {
			auto msg_copy = std::make_shared<voice_chat_message>(*recv_vc_msg_);
			auto buffer = boost::asio::buffer(msg_copy->data(), msg_copy->length());
			size_t size = msg_copy->length();
			//std::cout << "write to client " << iter->second->name << "\n";
			if (iter->second->id == msg_copy->sender_id && !iter->second->get_feedback_option()) { 
				//std::cout << "skip send same name\n";
				continue; }
			//std::cout << "do async send\n";
			udp_socket_->async_send_to(buffer, *iter->second->get_client_udp_endpoint(),
				[this, self, size, iter, msg_copy](boost::system::error_code ec, std::size_t bytes) {
					if (ec) {
						//std::cout << "write to client [" << iter->second->id << "][" << iter->second->name << "] fail\n";
					}
					else {
						/*std::cout << "write to client [" << static_cast<int>(iter->second->id) << "][" << iter->second->name << "] @ port " 
							<< " write to client size [" << size << "]\n\t" 
							<< iter->second->get_client_udp_endpoint()->port() << " ip " << iter->second->get_client_udp_endpoint()->address() << " success\n";*/
					}
				}
			);
			//std::cout << "async send complete\n";
		}
		//don't want to commit read for every person, but need to only commit after all sends have completed??
		//std::cout << "get sender\n";
		//std::cout << "at id " << static_cast<int>(sender_id) << "\n";
		auto sender = participants_[sender_id];
		//std::cout << "commit read of size " << recv_vc_msg_->body_length() << "\n";
		//std::cout << "send to " << sender->name << "\n";
		sender->commit_read_rb(recv_vc_msg_->body_length());
		//std::cout << "call read_vc_rb()\n";
		// duplicate call? already posting call to read_vc_rb within that method
		//sender->read_vc_rb();
		//std::cout << "call sender->read_vc_rb()\n";
		//adsfsdf
	}
	uint8_t room_id_;
	std::shared_ptr<udp::socket> udp_socket_;
	udp::endpoint& udp_endpoint_;
	udp::endpoint udp_remote_endpoint_;
	std::unordered_map<uint8_t, chat_participant_ptr> participants_; //thread safety mutex/strand
	boost::asio::io_context& io_context_;
	boost::asio::steady_timer timer_;
};

class chat_room : public std::enable_shared_from_this<chat_room> {
public:
	chat_room(std::shared_ptr<udp::socket>udp_socket, udp::endpoint& udp_endpoint, boost::asio::io_context& io_context): 
		udp_socket_{ std::move(udp_socket) }, udp_endpoint_{ udp_endpoint }, io_context_{ io_context } {
	}
	boost::asio::io_context& io_context_;
	chat_message_queue recent_msgs_;
	std::unordered_map<uint8_t, chat_participant_ptr> participant_map;
	std::unordered_map<uint8_t, std::shared_ptr<voice_chat_room>> vc_rooms;
	std::unordered_map<uint8_t, std::string> tokens;
	bool route_udp(uint8_t vc_room_id_, std::shared_ptr<voice_chat_message> recv_vc_msg_) {
		if (vc_rooms.find(vc_room_id_) != vc_rooms.end()) {
			//std::cout << "routing message to vc_room_id_[" << static_cast<int>(vc_room_id_) << "]\n";
			vc_rooms.at(vc_room_id_)->route_vc_msg_to_sender(recv_vc_msg_);
			return true;
		}
		return false;
	}
	bool validate_token(uint8_t sender_id_, std::string token_) {
		auto it = tokens.find(sender_id_);
		if (tokens.find(sender_id_) != tokens.end() && token_.size() == voice_chat_message::token_length &&
			it->second == token_) {
			//std::cout << "token[" << static_cast<int>(sender_id_) << "] = " << tokens[sender_id_] << "\n";
			return true;
		}
		return false;
	}
	void join(chat_participant_ptr participant) {
		std::cout << "participant w/ id " << static_cast<int>(participant->id) << " join room\n";
		if (duplicate_id(participant->id) || participant_map.size() >= max_participants) {
			return; }
		participant->id = generate_id();
		std::cout << "generate_id -> " << static_cast<int>(participant->id) << "\n";
		participant_map.insert(std::make_pair(
			participant->id, participant));
		tokens.insert(std::make_pair(participant->id, participant->session_token));
		chat_message id_msg;
		id_msg.set_message_type(message_type::send_id);
		id_msg.body_length(sizeof(uint8_t));
		std::memcpy(id_msg.body(), &(participant->id), id_msg.body_length());
		id_msg.encode_header();
		participant->deliver(id_msg);

		for (auto msg : recent_msgs_) {
			participant->deliver(msg);
		}
	}
	void send_vc_leave_notification(chat_participant_ptr participant) {
		uint8_t vc_room_id = participant->get_vc_room_id();
		auto iter = vc_rooms.find(vc_room_id);
		if (iter == vc_rooms.end()) { return; }
		uint8_t ids[max_participants];
		uint8_t count;
		vc_rooms.at(vc_room_id)->get_participant_ids(ids, count);
		chat_message msg;
		uint8_t sender_id = participant->id;
		msg.set_message_type(message_type::end_vc);
		std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
		std::memcpy(msg.body() + sizeof(sender_id), &count, sizeof(count));
		std::memcpy(msg.body() + sizeof(sender_id) + sizeof(count), ids, count);
		msg.body_length(sizeof(sender_id) + sizeof(count) + count);
		msg.encode_header();
		participant->deliver(msg);
	}
	//TODO use function pointer to combine this with send room leave notification
	void send_vc_leave_notifications(chat_participant_ptr participant) {
		uint8_t vc_room_id = participant->get_vc_room_id();
		auto iter = vc_rooms.find(vc_room_id);
		if (iter == vc_rooms.end()) { return; }
		auto p = vc_rooms.at(vc_room_id)->participants_.begin();
		for (; p != vc_rooms.at(vc_room_id)->participants_.end(); ++p) {
			chat_message msg;
			uint8_t sender_id = participant->id;
			msg.set_message_type(message_type::end_vc);
			std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
			msg.body_length(sizeof(sender_id));
			msg.encode_header();
			participant->deliver(msg);
		}
	}

	void send_room_leave_notifications(chat_participant_ptr participant) {
		auto iter = participant_map.find(participant->id);
		if (iter == participant_map.end()) { return; }
		auto p = participant_map.begin();
		for (; p != participant_map.end(); ++p) {
			chat_message msg;
			uint8_t sender_id = participant->id;
			msg.set_message_type(message_type::end_chat);
			std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
			msg.body_length(sizeof(sender_id));
			msg.encode_header();
			participant->deliver(msg);
		}
	}

	void leave_vc_room(chat_participant_ptr participant) {
		send_vc_leave_notifications(participant);
		uint8_t vc_room_id = participant->get_vc_room_id();
		std::cout << "leave room vc_room_id = " << static_cast<int>(vc_room_id) << "\n";
		uint8_t left_vc_room = 0;
		if (vc_room_id) {
			std::cout << "search for room\n";
			auto iter = vc_rooms.find(vc_room_id);
			if (iter != vc_rooms.end()) {
				vc_rooms.at(vc_room_id)->leave_room(participant);
				std::cout << "participant " << participant->name << " " << static_cast<int>(participant->id) << " removed from vc room " << static_cast<int>(vc_room_id) << "\n";

				if (vc_rooms.at(vc_room_id)->participants_.size() == 0) {
					vc_rooms.erase(vc_room_id);
					vc_room_id_gen.release_id(vc_room_id);
				}
			}
			std::cout << "end search\n";
		}
	}
	void leave(chat_participant_ptr participant) {
		leave_vc_room(participant);
		//leave any vc rooms first
		std::cout << "participant w/ id " << static_cast<int>(participant->id) << " leave chat room\n";
		//remove token
		if (!participant->id) { std::cout << "participant id is 0. not leaving room\n"; return; }
		send_room_leave_notifications(participant);
		auto it = tokens.find(participant->id);
		std::cout << "search for token @ participant id " << static_cast<int>(participant->id) << "\n";
		if (it != tokens.end()) {
			tokens.erase(participant->id);
		}
		//leave chat room
		release_id(participant->id);
		participant_map.erase(participant->id);
	}
	void deliver(const chat_message& msg) {
		recent_msgs_.push_back(msg);
		while (recent_msgs_.size() > max_recent_msgs)
			recent_msgs_.pop_front();
		std::unordered_map<uint8_t, chat_participant_ptr>::iterator it =
			participant_map.begin();
		for (; it != participant_map.end(); ++it) {
			it->second->deliver(msg);
		}
	}
	bool check_room_full() {
		if (next_id >= 255) {
			return true;
		}
		return false;
	}
	bool duplicate_id(uint8_t id) {
		std::unordered_map<uint8_t, chat_participant_ptr>::iterator it
			= participant_map.find(id);
		if (it != participant_map.end()) {

			return true;
		}
		return false;
	}
	uint8_t serialize_participant(chat_participant_ptr p, char* ptr) {
		uint8_t p_length_2 = 0;
		p_length_2 += sizeof(uint8_t);//id
		p_length_2 += sizeof(uint8_t);//vc
		std::string name = p->name;
		uint8_t name_length = name.length();
		p_length_2 += sizeof(name_length);
		p_length_2 += name_length;

		std::memcpy(ptr, &p_length_2, sizeof(uint8_t));
		std::memcpy(ptr + sizeof(uint8_t) * 1, &p->id, sizeof(uint8_t));
		uint8_t state = static_cast<uint8_t>(p->vc_state);
		std::memcpy(ptr + sizeof(uint8_t) * 2, &state, sizeof(uint8_t));
		std::memcpy(ptr + sizeof(uint8_t) * 3, &name_length, sizeof(uint8_t));
		std::memcpy(ptr + sizeof(uint8_t) * 4, name.c_str(), name.length());

		return 4 + name_length;
	}
	void update_client_participants() {
		uint8_t ps = participant_map.size();
		uint8_t t_length_2 = 0;
		uint8_t p_length_2 = 0;
		t_length_2 += sizeof(ps);
		std::unordered_map<uint8_t, chat_participant_ptr>::iterator it =
			participant_map.begin();
		chat_message msg_test_2;
		msg_test_2.set_message_type(message_type::particpants_request);
		std::memcpy(msg_test_2.body(), &ps, sizeof(ps));//# participants
		int i = 1;
		for (; it != participant_map.end(); ++it) {
			i += serialize_participant(it->second, msg_test_2.body() + sizeof(uint8_t) * i);
			t_length_2 += i;
		}
		msg_test_2.body_length(t_length_2);
		msg_test_2.encode_header();

		it =
			participant_map.begin();
		for (; it != participant_map.end(); ++it) {
			it->second->deliver(msg_test_2);
		}
	}
	//TODO: move this to chat_session
	void send_vc_request(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), sizeof(uint8_t));//sender id is always first
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + sizeof(uint8_t), sizeof(uint8_t));//followed by receiver id
		chat_message request;
		request.msg_type = message_type::recieve_vc_request;
		//uint8_t length = serialize_participant(participant_map.at(sender_id), request.body());
		std::memcpy(request.body(), &sender_id, sizeof(sender_id));
		request.body_length(sizeof(sender_id));
		request.encode_header();
		participant_map.at(receiver_id)->deliver(request);
	}
	//TODO: move this to chat_session
	void reject_vc_request(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), sizeof(uint8_t));//sender id is always first
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + sizeof(uint8_t), sizeof(uint8_t));//followed by receiver id
		chat_message request;
		request.msg_type = message_type::reject_vc_request;
		//uint8_t length = serialize_participant(participant_map.at(sender_id), request.body());
		std::memcpy(request.body(), &sender_id, sizeof(sender_id));
		request.body_length(sizeof(sender_id));
		request.encode_header();
		participant_map.at(sender_id)->deliver(request);
	}
	void accept_vc_request(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), sizeof(uint8_t));//sender id is always first
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + sizeof(uint8_t), sizeof(uint8_t));//followed by receiver id
		std::cout << "accept_vc_request(): sender_id[" << static_cast<int>(sender_id) << "] receiver_id[" << static_cast<int>(receiver_id) << "]\n";
		chat_message msg;
		uint8_t vc_room_id = create_vc_room();
		std::cout << "create vc_room_id " << static_cast<int>(vc_room_id) << "\n";
		vc_rooms.at(vc_room_id)->join_room(participant_map[sender_id]);
		vc_rooms.at(vc_room_id)->join_room(participant_map[receiver_id]);
		msg.msg_type = message_type::accept_vc_request;	
		uint8_t ids[max_participants];
		uint8_t count;
		vc_rooms.at(vc_room_id)->get_participant_ids(ids, count);
		std::memcpy(msg.body(), &count, sizeof(count));
		std::memcpy(msg.body() + sizeof(count), ids, count);
		std::memcpy(msg.body() + sizeof(count) + count, &vc_room_id, sizeof(vc_room_id));
		std::memcpy(msg.body() + sizeof(count) + count + sizeof(vc_room_id), &udp_port, sizeof(udp_port));//2bytes
		msg.body_length(sizeof(count) + count + sizeof(vc_room_id) + sizeof(udp_port));
		msg.encode_header();
		//std::string header = std::string(msg.data(), chat_message::header_length);
		//std::string body = std::string(msg.body(), msg.body_length());
		//std::cout << "accept_vc_request count[" << static_cast<int>(count) << "] header[" << header << "] body[" << body << "]\n";
		approved_vcs.emplace(sender_id, receiver_id);
		if (sender_id != receiver_id) {
			approved_vcs.emplace(receiver_id, sender_id);
		}
		participant_map.at(sender_id)->deliver(msg);
		if (sender_id != receiver_id) {
			participant_map.at(receiver_id)->deliver(msg);
		}
	}
	void accept_mic_check_request(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), sizeof(uint8_t));//sender id is always first
		chat_message msg;
		uint8_t vc_room_id = create_vc_room();
		std::cout << "create vc_room_id " << static_cast<int>(vc_room_id) << "\n";
		vc_rooms.at(vc_room_id)->join_room(participant_map[sender_id]);
		msg.msg_type = message_type::mic_test;
		std::memcpy(msg.body(), &sender_id, 1);
		std::memcpy(msg.body() + sizeof(sender_id), &vc_room_id, sizeof(vc_room_id));
		std::memcpy(msg.body() + sizeof(sender_id) + sizeof(vc_room_id), &udp_port, sizeof(udp_port));//2bytes
		msg.body_length(+sizeof(sender_id) + sizeof(vc_room_id) + sizeof(udp_port));
		msg.encode_header();
		std::string header = std::string(msg.data(), chat_message::header_length);
		std::string body = std::string(msg.body(), msg.body_length());
		//std::cout << "accept_vc_request count[" << static_cast<int>(count) << "] header[" << header << "] body[" << body << "]\n";
		approved_vcs.emplace(sender_id, sender_id);
		participant_map.at(sender_id)->deliver(msg);
	}
	void get_room_participant_ids(uint8_t room_id, uint8_t* ids, uint8_t& count) {
		count = 0;
		auto iter = vc_rooms.at(room_id)->participants_.begin();
		uint8_t id_size = sizeof(chat_participant::id);
		for (; iter != vc_rooms.at(room_id)->participants_.end() && count < max_participants; ++iter) {
			uint8_t id = iter->second->id;
			std::memcpy(ids + count * id_size, &id, id_size);
			++count;
		}
	}
	uint8_t create_vc_room() {
		uint8_t id = vc_room_id_gen.generate_id();
		auto room = std::make_shared<voice_chat_room>(udp_socket_, udp_endpoint_, io_context_, id);
		vc_rooms.emplace(id, room);//TODO dunno if 'this' works
		return id;
	}
private:
	std::unordered_map<uint8_t, uint8_t> approved_vcs;//sender_id, partner_id
	std::shared_ptr<udp::socket> udp_socket_;
	udp::endpoint& udp_endpoint_;
	enum {max_recent_msgs = 100};
	id_generator vc_room_id_gen{};
	ma_rb ring_buffer_;
};

struct endpoint_hash {
	std::size_t operator()(const udp::endpoint& ep) const {
		return std::hash<std::string>()(ep.address().to_string()) ^
			std::hash<unsigned short>()(ep.port());
	}
};
struct endpoint_equal {
	bool operator()(const udp::endpoint& lhs, const udp::endpoint& rhs) const {
		return lhs.address() == rhs.address() && lhs.port() == rhs.port();
	}
};
class chat_session :
	public chat_participant,
	public std::enable_shared_from_this<chat_session>
{
public:
	tcp::endpoint tcp_endpoint_;
	std::string client_ip_;
	unsigned short client_port_;

	enum class session_state {
		wait,
		handshake,
		awaiting_name,
		active
	};
	chat_session(boost::asio::ssl::stream<tcp::socket> ssl_socket, tcp::socket socket, tcp::endpoint tcp_endpoint, std::shared_ptr<udp::socket> udp_socket,
		udp::endpoint udp_endpoint, unsigned short client_port, std::shared_ptr<chat_room> room, boost::asio::io_context& io_context) :
		ssl_socket_(std::move(ssl_socket)),
		socket_(std::move(socket)),
		tcp_endpoint_{tcp_endpoint},
		udp_socket_{udp_socket},
		client_ip_{ tcp_endpoint.address().to_string()},
		client_port_{client_port},
		room_(room),
		authenticated(false),
		io_context{io_context},
		vc_room_id(0)
	{
	}

	bool get_feedback_option() override {
		return feedback;
	}
	void commit_read_rb(size_t size) override{
		ma_rb_commit_read(&vc_rb, size);
	}
	std::shared_ptr<boost::asio::ip::udp::endpoint> get_client_udp_endpoint() override {
		return udp_remote_endpoint_;
	}
	uint8_t get_vc_room_id() override {
		return vc_room_id;
	}
	void set_vc_room_id(uint8_t id) override {
		vc_room_id = id;
	}
	void send_authentication_request() {
		chat_message auth;
		std::string text = "gimme auth";
		auth.body_length(text.length());
		auth.set_message_type(message_type::authentication_request);
		std::memcpy(auth.body(), text.data(), text.length());
		auth.encode_header();
		deliver(auth);
	}


	void wait_for_ready() {
		state_ = session_state::wait;
		authenticated = false;
		id = 0;
		//do_read_header(); 
		do_read_header_ssl();
	}

	void start_ssl() {
		do_handshake();
	}

	void start() {
		chat_message prompt;
		std::string text = "Enter name:";
		prompt.body_length(text.length());
		prompt.set_message_type(message_type::name_challenge);
		std::memcpy(prompt.body(), text.data(), prompt.body_length());
		prompt.encode_header();
		deliver(prompt);
	}
	void reject() {
		chat_message prompt;
		std::string text = "room full";
		prompt.body_length(text.length());
		prompt.set_message_type(message_type::reject_join);
		std::memcpy(prompt.body(), text.data(), prompt.body_length());
		prompt.encode_header();
		deliver(prompt);
	}
	bool init_rb(){
		ma_uint32 bpf;
		ma_uint32 subBufferSizeInFrames;
		//subBufferSizeInFrames = deviceCapture.capture.internalPeriodSizeInFrames * 5;
		//bpf = ma_get_bytes_per_frame(deviceCapture.capture.format, deviceCapture.capture.channels);
		//rb  size in bytes = 19200
		//just set to 19200 for now. eventually will need to calculate size
		ma_result result = ma_rb_init(19200, NULL, NULL, &vc_rb);
		if (result != MA_SUCCESS) {
			std::cout << "Failed to initialize capture ring buffer\n";
			return false;
		}
		return true;
	}
	void uninit_rb() {
		ma_rb_uninit(&vc_rb);
	}
	void write_vc_msg_to_rb(std::shared_ptr<voice_chat_message> recv_vc_msg_) override {
	//	std::cout << "write_vc_msg_to_rb()\n";
		void* pwrite_void = nullptr;
		size_t size = recv_vc_msg_->body_length();
		size_t requested = size;
		size_t total_written = 0;
		ma_result result;
		uint8_t* data = (uint8_t*)recv_vc_msg_->body();
		while (requested > 0) {
			size = requested;
			//std::cout << "write size request = " << size << "\n";
			result = ma_rb_acquire_write(&vc_rb, &size, &pwrite_void);
			//std::cout << "actual write size = " << size << "\n";
			if (result != MA_SUCCESS || size == 0) {
				std::cerr << "fail server write acquire size = " << size << "\n";
				break;
			}
			std::memcpy(pwrite_void, data + total_written, (uint16_t)size);
			ma_rb_commit_write(&vc_rb, size);
			requested -= size;
			total_written += size;
		}
		//std::cout << "total_written = " << total_written << "\n";
	}
	bool try_send_vc_msg(void* in, size_t size) {
		//std::cout << "try_send_vc_msg()\n";
		std::shared_ptr<voice_chat_message> m = std::make_shared<voice_chat_message>();
		uint8_t test = 3;
		/*std::cout << "encoding:"
			<< "\n\tsession_token[" << session_token << "]"
			<< "\n\tsize[" << static_cast<int>(size) << "]"
			<< "\n\tvc_room_id[" << static_cast<int>(vc_room_id) << "]"
			<< "\n\tid[" << static_cast<int>(id) << "]"
			<< "\n\ttest[" << static_cast<int>(test) << "]\n";*/
		m->encode_header(session_token, size, vc_room_id, id);
		std::memcpy(m->body(), in, size);
		if (room_->vc_rooms.find(vc_room_id) == room_->vc_rooms.end()) {
			std::cerr << "chat_participant " << id << " vc_room_id " << vc_room_id << "doesn't exist\n";
			return false;
		}
		/*if (room_->vc_rooms[vc_room_id]->participants_.size() <= 1) { 
			std::cout << "room only has 1 participant not sending msg\n";
			return false; }*/
		//std::cout << "send_message_to_playback()\n";
		room_->vc_rooms[vc_room_id]->send_message_to_playback(m, id);
		return true;
	}
	void start_read_vc_rb() override {
		running_vc = true;
		init_rb();
		auto self = shared_from_this();
		boost::asio::post(io_context, [this, self] { read_vc_rb(); });
	}
	void stop_read_vc_rb() override {
		running_vc = false;
		uninit_rb();
	}
	void read_vc_rb() override {
		if (!running_vc) { return; }
		auto self = shared_from_this();
		ma_result result;
		size_t size = voice_chat_message::buffer_size;
		void* pOut;
		//std::cout << "requested read size[" << size << "]\n";
		result = ma_rb_acquire_read(&vc_rb, &size, &pOut);
		if (result != MA_SUCCESS || size == 0) {
			//std::cerr << "fail server read acquire " << result
				//<< "\n\tserver read size available = " << ma_rb_available_read(&vc_rb) << "\n";
			boost::asio::post(io_context, [this, self] { read_vc_rb(); });
			return;
		}
		//std::cout << "actual read size[" << size << "]\n";
		if (size > voice_chat_message::max_body_length) { size = voice_chat_message::max_body_length; }
		if (!try_send_vc_msg(pOut, size)) {
			/*auto retry_timer = std::make_shared <boost::asio::steady_timer>(io_context);
			retry_timer->expires_after(std::chrono::milliseconds(2));
			retry_timer->async_wait([this, self, retry_timer](boost::system::error_code ec) {
				if (!ec) {
					read_vc_rb();
				}
			});*/
			//shut down voice chat TODO
			return;
		}
		//std::cout << "try send done re posting read_vc_rb\n";
		boost::asio::post(io_context, [this, self] { read_vc_rb(); });
	}
	void deliver(const chat_message& msg) override
	{
		bool write_in_progress = !write_msgs_.empty();
		write_msgs_.push_back(msg);
		if (!write_in_progress)
		{
			//std::cout << "do_write()\n";
			//do_write();
			do_write_ssl();
		}
	}

private:
	std::string generate_token() {
		std::array<uint8_t, 12> bytes;
		std::random_device rd;
		std::generate(bytes.begin(), bytes.end(), std::ref(rd));

		using namespace boost::archive::iterators;
		using It = base64_from_binary<transform_width<const uint8_t*, 6, 8>>;

		std::string token(It(bytes.data()), It(bytes.data() + bytes.size()));
		return token; // Exactly 16 characters, no padding needed
	}
	bool verify_authorization_response(chat_message& msg) {
		std::string p = std::string(msg.body(), msg.body_length());
		if (p.find(key) != std::string::npos) {
			//std::cout << "auth verified!!!\n";
			session_token = generate_token();
			std::cout << "session_token[" << session_token << "]\n";
			return true;
		}
		std::cout << "auth not verified\n";
		return false;
	}
	void store_client_udp_port(chat_message& msg) {
		std::cout << "store!\n";
		//uint16_t port = 0;
		//if (msg.body_length() < sizeof(port)) {
			//header_not_recognized_error();
			//todo cancel out vc request for both clients
			//return;
		//}
		//std::cout << "no bad header\n";
		//std::memcpy(&port, msg.body(), sizeof(port));
		std::string port_2(msg.body(), msg.body_length());
		//std::cout << "udp port = " << port << "\n";
		std::cout << "udp port_2 = " << port_2 << "\n";
		std::cout << "test\n";
		//udp_remote_endpoint_ = std::make_shared<udp::endpoint>(client_ip, port);
		udp_remote_endpoint_ = std::make_shared<udp::endpoint>(tcp_endpoint_.address(), std::stoi(port_2));
		//std::shared_ptr<boost::asio::ip::udp::endpoint> ep_2 = std::make_shared<udp::endpoint>(client_ip, std::stoi(port_2));
		std::cout << "udp_remote_endpoint port = " << static_cast<int>(udp_remote_endpoint_->port()) << "\n";
		//std::cout << "udp_remote_endpoint port_2 = " << (ep_2->port()) << "\n";//TODO use this endpoint bc sending port as char not bytes
		chat_message response;
		response.body_length(session_token.length());
		response.set_message_type(message_type::start_vc);
		std::memcpy(response.body(), session_token.c_str(), session_token.length());
		response.encode_header();
		//std::cout << "send udp\n";
		deliver(response);
	}
	void header_not_recognized_error() {
		std::string text = "error\n";
		chat_message bad_header;
		bad_header.body_length(text.length());
		bad_header.set_message_type(message_type::bad_message);
		std::memcpy(bad_header.body(), text.data(), text.length());
		bad_header.encode_header();
		deliver(bad_header);
	}
	void change_name(std::string name) {
		this->name = name;
		state_ = session_state::active;
		std::string text = "Welcome";
		chat_message validation;
		validation.body_length(text.length());
		validation.set_message_type(message_type::name_approve);
		std::memcpy(validation.body(), text.c_str(), validation.body_length());
		validation.encode_header();
		deliver(validation);
	}
	void do_handshake() {
		//std::cout << "do_handshake()\n";
		auto self(shared_from_this());
		//std::cout << "start handshake\n";
		ssl_socket_.async_handshake(boost::asio::ssl::stream_base::server,
			[this, self](const boost::system::error_code& ec) {
				if (!ec) {
					//std::cout << "handshake success\n";
					wait_for_ready();
				}
				else {
					std::cout << "handshake fail: " << ec.message() << "\n";
				}
		});
	}
	void do_read_header_ssl() {
		//std::cout << "do_read_header_ssl\n";
		auto self(shared_from_this());
		boost::asio::async_read(ssl_socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this, self](boost::system::error_code ec, std::size_t)
			{
				if (!ec && read_msg_.decode_header()) {
					//std::string header = std::string(read_msg_.data(), chat_message::header_length);
					//std::cout << "read_msg_header[" << header << "]\n";					
					do_read_body_ssl();

				}
				else {
					std::cerr << "read error: " << ec.message() << "\n";
					room_->leave(shared_from_this());
				}
			});
	}
	void do_read_header() {
		std::cout << "do_read_header()\n";
		auto self(shared_from_this());
		boost::asio::async_read(socket_,
			boost::asio::buffer(read_msg_.data(), chat_message::header_length),
			[this, self](boost::system::error_code ec, std::size_t)
			{
				if (!ec && read_msg_.decode_header()) {
					//std::string header = std::string(read_msg_.data(), chat_message::header_length);
					//std::cout << "read_msg_header[" << header << "]\n";					
					do_read_body();
					
				}
				else {
					std::cerr << "read error: " << ec.message() << "\n";
					room_->leave(shared_from_this());
				}
		});
	}
	void do_read_body_ssl() {
		//std::cout << "do_read_body_ssl()\n";
		auto self(shared_from_this());
		boost::asio::async_read(ssl_socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this, self](boost::system::error_code ec, std::size_t)
			{
				if (!ec) {
					std::string header = std::string(read_msg_.data(), chat_message::header_length);
					std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header[" << header << "] body [" << body << "]\n";
					//validate client is authorized!! TODO
					if (!authenticated) {
						if (verify_authorization_response(read_msg_)) {
							chat_message m;
							m.body_length(session_token.length());//16 bytes/chars
							m.set_message_type(message_type::authentication_approve);
							std::memcpy(m.body(), session_token.c_str(), m.body_length());
							m.encode_header();
							deliver(m);
							authenticated = true;
						}
						else {
							send_authentication_request();
							authenticated = false;
							//room_->leave(shared_from_this());
						}
						do_read_header_ssl();
						return;

					}
					switch (read_msg_.msg_type) {
						/*case(message_type::ready_notification): {
							if (state_ == session_state::wait) {
								send_authentication_request();
							}
							break;
						}*/
	
					case(message_type::start_room_request): {
						if (authenticated) {
							if (room_->check_room_full()) {
								reject();
							}
							else {
								start();
							}
						}
						break;
					}
					case(message_type::name_change_request): {
						state_ = session_state::awaiting_name;
						//name is preceded by uint8_t id
						std::string name(read_msg_.body() + sizeof(uint8_t), read_msg_.body_length() - sizeof(uint8_t));
						room_->leave(shared_from_this());
						std::cout << "new name = " << name << "\n";
						if (!name.empty()) {
							uint8_t id = 0;
							std::memcpy(&id, read_msg_.body(), sizeof(uint8_t));
							change_name(name);
							//std::cout << "name changed\n";
							this->id = id;
							room_->join(shared_from_this());
							room_->update_client_participants();
						}
					}
														   break;
					case(message_type::chat): {
						std::string full_msg = name + ": ";
						full_msg.append(read_msg_.body(), read_msg_.body_length());
						read_msg_.body_length(full_msg.length());
						memcpy(read_msg_.body(), full_msg.c_str(), read_msg_.body_length());
						read_msg_.encode_header();
						room_->deliver(read_msg_);
						break;
					}
					case(message_type::send_vc_request): {
						room_->send_vc_request(read_msg_);
						break;
					}
					case(message_type::reject_vc_request): {
						room_->reject_vc_request(read_msg_);
						break;
					}
					case(message_type::accept_vc_request): {
						room_->accept_vc_request(read_msg_);
						break;
					}
					case(message_type::send_udp_port): {
						//std::cout << "got send_udp_port\n";
						store_client_udp_port(read_msg_);//TODO code review this
						break;
					}
					case(message_type::mic_test): {
						room_->accept_mic_check_request(read_msg_);
						break;
					}
					case(message_type::end_vc): {
						//room_->send_vc_leave_notification(shared_from_this());
						//room_->send_vc_leave_notifications(shared_from_this());
						room_->leave_vc_room(shared_from_this());
						break;
					}

					}
					do_read_header_ssl();
				}
				else {
					std::cerr << "Write error: " << ec.message() << "\n";
					room_->leave(shared_from_this());
					authenticated = false;
				}
			}
		);
	}
	void do_read_body() {
		std::cout << "do_read_body()\n";
		auto self(shared_from_this());
		boost::asio::async_read(socket_,
			boost::asio::buffer(read_msg_.body(), read_msg_.body_length()),
			[this, self](boost::system::error_code ec, std::size_t)
			{
				std::cout << "do_read_body async_read() shouldn't be happening\n";
				if (!ec) {
					std::string header = std::string(read_msg_.data(), chat_message::header_length);
					std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header[" << header << "] body [" << body << "]\n";
					//validate client is authorized!! TODO
					if (!authenticated) {
						if(verify_authorization_response(read_msg_)){
							chat_message m;
							m.body_length(session_token.length());//16 bytes/chars
							m.set_message_type(message_type::authentication_approve);
							std::memcpy(m.body(), session_token.c_str(), m.body_length());
							m.encode_header();
							deliver(m);
							authenticated = true;
						}
						else {
							send_authentication_request();
							authenticated = false;
							//room_->leave(shared_from_this());
						}
						do_read_header();
						return;

					}
					switch (read_msg_.msg_type) {
						/*case(message_type::ready_notification): {
							if (state_ == session_state::wait) {
								send_authentication_request();
							}
							break;
						}*/

						case(message_type::start_room_request): {
							if (authenticated) {
								if (room_->check_room_full()) {
									reject();
								}
								else {
									start();
								}
							}
							break;
						}
						case(message_type::name_change_request): {
							state_ = session_state::awaiting_name;
							//name is preceded by uint8_t id
							std::string name(read_msg_.body() + sizeof(uint8_t), read_msg_.body_length() - sizeof(uint8_t));
							room_->leave(shared_from_this());
							if (!name.empty()) {
								uint8_t id = 0;
								std::memcpy(&id, read_msg_.body(), sizeof(uint8_t));
								change_name(name);
								//std::cout << "name changed\n";
								this->id = id;
								room_->join(shared_from_this());
								room_->update_client_participants();
							}
						}
							break;
						case(message_type::chat): {
							std::string full_msg = name + ": ";
							full_msg.append(read_msg_.body(), read_msg_.body_length());
							read_msg_.body_length(full_msg.length());
							memcpy(read_msg_.body(), full_msg.c_str(), read_msg_.body_length());
							read_msg_.encode_header();
							room_->deliver(read_msg_);
							break;
						}
						case(message_type::send_vc_request): {
							room_->send_vc_request(read_msg_);
							break;
						}
						case(message_type::reject_vc_request): {
							room_->reject_vc_request(read_msg_);
							break;
						}
						case(message_type::accept_vc_request): {	
							room_->accept_vc_request(read_msg_);
							break;
						}
						case(message_type::send_udp_port): {
							//std::cout << "got send_udp_port\n";
							store_client_udp_port(read_msg_);//TODO code review this
							break;
						}
						case(message_type::mic_test): {
							room_->accept_mic_check_request(read_msg_);
							break;
						}
						case(message_type::end_vc):{
							//room_->send_vc_leave_notification(shared_from_this());
							//room_->send_vc_leave_notifications(shared_from_this());
							room_->leave_vc_room(shared_from_this());
							break;
						}
															
					}
					do_read_header();
				}
				else {
					std::cerr << "Write error: " << ec.message() << "\n";
					room_->leave(shared_from_this());
					authenticated = false;
				}
			}
		);
	}
	void do_write_ssl() {
		//std::cout << "do_write_ssl()\n";
		auto self(shared_from_this());
		auto msg = write_msgs_.front();
		boost::asio::async_write(ssl_socket_,
			//boost::asio::buffer(write_msgs_.front().data(), 
			//write_msgs_.front().length()),
			boost::asio::buffer(msg.data(),
				msg.length()),
			[this, self, msg](boost::system::error_code ec, std::size_t)
			{
				if (!ec) {
					std::string header = std::string(msg.data(), chat_message::header_length);
					std::string body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
					//std::cout << "write msg header[" << header << "] body [" << body << "]\n";
						//std::cout << "!ec\n";
					write_msgs_.pop_front();
					if (!write_msgs_.empty()) {
						do_write_ssl();
					}
				}
				else {
					std::cerr << "Write error: " << ec.message() << "\n";
					room_->leave(shared_from_this());
				}
			}
		);
	}
	void do_write(){
		std::cout << "do_write()\n";
		auto self(shared_from_this());
		auto msg = write_msgs_.front();
		boost::asio::async_write(socket_,
			//boost::asio::buffer(write_msgs_.front().data(), 
			//write_msgs_.front().length()),
			boost::asio::buffer(msg.data(),
			msg.length()),
			[this, self, msg](boost::system::error_code ec, std::size_t)
			{
				std::cout << "do_write() async_write() should not be happening\n";

				if (!ec) {
					std::string header = std::string(msg.data(), chat_message::header_length);
					std::string body = std::string(write_msgs_.front().body(), write_msgs_.front().body_length());
					//std::cout << "write msg header[" << header << "] body [" << body << "]\n";
						//std::cout << "!ec\n";
						write_msgs_.pop_front();
						if (!write_msgs_.empty()) {
							do_write();
						}
					}
					else {
						std::cerr << "Write error: " << ec.message() << "\n";
						room_->leave(shared_from_this());
					}
			}
		);
	}
	boost::asio::ssl::stream<tcp::socket> ssl_socket_;
	tcp::socket socket_;
	std::shared_ptr<chat_room> room_;
	chat_message read_msg_;
	chat_message_queue write_msgs_;
	session_state state_ = session_state::wait;
	bool authenticated = false;
	bool running_vc = false;
	bool feedback = true;
	//std::string session_token;//16 characters
	boost::asio::ip::address client_ip;
	std::shared_ptr<udp::socket> udp_socket_;
	std::shared_ptr<udp::endpoint> udp_remote_endpoint_;//make value type?
	ma_rb vc_rb;
	boost::asio::io_context& io_context;
	uint8_t vc_room_id;
};
class chat_server {
public:
	chat_server(boost::asio::io_context& io_context,
		const tcp::endpoint& endpoint, udp::endpoint& udp_endpoint) : acceptor_(io_context, endpoint),
		udp_socket_(std::make_shared<udp::socket>(io_context, udp_endpoint)),
		udp_endpoint_(udp_endpoint), room_(std::make_shared<chat_room>(udp_socket_, udp_endpoint_, io_context)),
		io_context_{ io_context }, ssl_context_{boost::asio::ssl::context::tls_server}
	{
		ssl_context_.set_options(
			boost::asio::ssl::context::default_workarounds
			| boost::asio::ssl::context::no_sslv2
			| boost::asio::ssl::context::no_sslv3
			| boost::asio::ssl::context::no_tlsv1
			| boost::asio::ssl::context::no_tlsv1_1
		);
		//only needed for encrypted private keys. let's encrypt not private key is not encrypted.
		//ssl_context_.set_password_callback(std::bind(&chat_server::get_password, this));
		ssl_context_.use_certificate_chain_file("/etc/letsencrypt/live/magoogan.duckdns.org/fullchain.pem");
		ssl_context_.use_private_key_file("/etc/letsencrypt/live/magoogan.duckdns.org/privkey.pem", boost::asio::ssl::context::pem);
		//do_accept();
		do_accept_ssl();
		//do_receive();
	}
	void do_receive() {
		if (udp_socket_ != nullptr) {
			udp_start_receive();
		}
	}
private:
	std::string get_password() const {
		return "test";
	}
	void do_accept_ssl() {
		acceptor_.async_accept(
			[this](const boost::system::error_code& ec, tcp::socket socket) {
				tcp::endpoint remote_ep = socket.remote_endpoint();
				boost::asio::ip::address ip = remote_ep.address();
				std::string client_ip = ip.to_string();
				unsigned short client_port = remote_ep.port();
				std::cout << "tcp connection from [" << client_ip << "] on port[" << client_port << "]\n";
				if (!ec) {
					std::make_shared<chat_session>(boost::asio::ssl::stream<tcp::socket>(std::move(socket), ssl_context_),
						std::move(socket), remote_ep, udp_socket_, udp_endpoint_, client_port, room_, io_context_)->start_ssl();
				}
				do_accept_ssl();
			}
			);
	}
	void do_accept() {
		acceptor_.async_accept(
			[this](boost::system::error_code ec, tcp::socket socket) 
			{
				tcp::endpoint remote_ep = socket.remote_endpoint();
				boost::asio::ip::address ip = remote_ep.address();
				std::string client_ip = ip.to_string();
				unsigned short client_port = remote_ep.port();
				std::cout << "tcp connection from [" << client_ip << "] on port[" << client_port << "]\n";
				if (!ec) {
					//std::make_shared<chat_session>(std::move(socket), remote_ep, udp_socket_, udp_endpoint_, client_port, room_, io_context_)->wait_for_ready();
				}
				do_accept();
			}); 
	}
	void udp_start_receive() {
		auto recv_vc_msg_ = std::make_shared<voice_chat_message>();
		udp_socket_->async_receive_from(
			boost::asio::buffer(recv_vc_msg_->data(), voice_chat_message::header_length + voice_chat_message::max_body_length), 
			udp_remote_endpoint_,
			std::bind(&chat_server::handle_receive, this,
				udp_remote_endpoint_,
				recv_vc_msg_,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
	}
	void udp_start_receive_old() {
		auto recv_vc_msg_ = std::make_shared<voice_chat_message>();
		udp_socket_->async_receive_from(
			boost::asio::buffer(recv_vc_msg_->data(), voice_chat_message::header_length + voice_chat_message::max_body_length), udp_remote_endpoint_,
			[this, recv_vc_msg_](boost::system::error_code ec, std::size_t bytes_recvd) {
				if (!ec) {
					if (recv_vc_msg_->decode_header()) {
						handle_receive_old(udp_remote_endpoint_, recv_vc_msg_);
					}
				}
				udp_start_receive_old();
			});
	}
	void handle_receive(udp::endpoint remote_endpoint, std::shared_ptr<voice_chat_message> recv_vc_msg_, const boost::system::error_code& error,
		std::size_t) {
		udp_start_receive();
		if (!error) {
			if (recv_vc_msg_->decode_header()) {
				std::string client_ip = remote_endpoint.address().to_string();
				unsigned short client_port = remote_endpoint.port();
				std::string key = client_ip + " : " + std::to_string(client_port);
				if (ips.insert(key).second) {
					std::cout << "new udp connection from [" << client_ip << "] on port [" << client_port << "]\n";
				}
				bool token_valid = room_->validate_token(recv_vc_msg_->sender_id, recv_vc_msg_->token);
				if (recv_vc_msg_->token_len == voice_chat_message::token_length && token_valid) {
					if (!room_->route_udp(recv_vc_msg_->room_id, recv_vc_msg_)) {
						std::cerr << "room " << recv_vc_msg_->room_id << " not found\n";
					}
				}
				else {
					std::cerr << "bad token: token_len[" << static_cast<int>(recv_vc_msg_->token_len) << "] validate_token[" << token_valid << "]\n";
				}
			}
		}
	}
	bool handle_receive_old(udp::endpoint remote_endpoint,std::shared_ptr<voice_chat_message> recv_vc_msg_) {
		//std::cout << "udp recieve\n";
		std::string client_ip = remote_endpoint.address().to_string();
		unsigned short client_port = remote_endpoint.port();
		std::string key = client_ip + " : " + std::to_string(client_port);
		if (ips.insert(key).second) {
			std::cout << "new udp connection from [" << client_ip << "] on port [" << client_port << "]\n";
		}
		//std::cout << "udp connection from [" << client_ip << "] on port [" << client_port << "]\n";

		bool token_valid = room_->validate_token(recv_vc_msg_->sender_id, recv_vc_msg_->token);
		if (recv_vc_msg_->token_len == voice_chat_message::token_length && token_valid) {
			if(!room_->route_udp(recv_vc_msg_->room_id, recv_vc_msg_)){
				std::cout << "search for room for udp connection from [" << client_ip << "] on port [" << client_port << "]\n";
				std::cerr << "room " << static_cast<int>(recv_vc_msg_->room_id) << " not found\n";
				return false;
			}
		}
		else {
			std::cerr << "bad token: token_len[" << static_cast<int>(recv_vc_msg_->token_len) << "] validate_token[" << token_valid << "]\n";
			return false;
		}
		//std::cout << "accept udp connection w/ token: " << recv_vc_msg_->token << "\n";
		return true;
	}
	boost::asio::ssl::context ssl_context_;
	boost::asio::io_context& io_context_;
	tcp::acceptor acceptor_;
	std::shared_ptr<udp::socket> udp_socket_;
	udp::endpoint& udp_endpoint_;
	udp::endpoint udp_remote_endpoint_;
	std::shared_ptr<chat_room> room_;
	std::unordered_set<std::string> ips;
};

int main(int argc, char* argv[]){
	try{
		if (argc < 2) {
			std::cerr << "Usage: chat_server <tcp_port> [<port> ...]\n";
			return 1;
		}

		boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13);
		boost::asio::io_context io_context;
		
		ssl_ctx.set_options(
			boost::asio::ssl::context::default_workarounds |
			boost::asio::ssl::context::no_sslv2 |
			boost::asio::ssl::context::single_dh_use
		);

		std::list<chat_server> servers;
		udp::endpoint udp_endpoint(udp::v4(), udp_port);
		for (int i = 1; i < argc; ++i) {
			tcp::endpoint endpoint(tcp::v4(), std::atoi(argv[i]));
			servers.emplace_back(io_context, endpoint, udp_endpoint);
		}
		io_context.run();
	}
	catch(std::exception& e){
		std::cerr << "Exception: " << e.what() << "\n";
	}
	return 0;
}

/*int main() {
	return 0;
}*/