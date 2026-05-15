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

#include "Typedefs.h"

using boost::asio::ip::tcp;
using boost::asio::ip::udp;
typedef std::deque<chat_message> chat_message_queue;
typedef std::shared_ptr<chat_participant> chat_participant_ptr;
typedef std::deque<voice_chat_message> vc_message_queue;
const std::string key = "basic_password_authorization:D";
const uint8_t key_length = 30;
const std::string current_version = "1.0";
const uint8_t current_version_length = 3;
const uint8_t max_participants = 1;
const uint8_t max_name_length = 16;
//typedef std::shared_ptr<chat_participant> chat_participant_ptr;
typedef std::shared_ptr<voice_chat_participant> vc_participant_ptr;
std::queue<uint8_t> free_ids{};
uint8_t next_id = 1;
constexpr size_t packet_size = 512;
constexpr size_t packet_count = 64;

constexpr uint16_t udp_port = 5000;
const int buffer_size = 38400; //19200;


struct pair_hash {
	size_t operator() (const std::pair<uint8_t, uint8_t>& p) const {
		uint8_t a = std::min(p.first, p.second);
		uint8_t b = std::max(p.first, p.second);
		return (static_cast<size_t>(a) << 8) | b;
	}
};

struct pair_equality{
	bool operator ()(const std::pair<uint8_t, uint8_t>& p1, 
		const std::pair<uint8_t, uint8_t>& p2) const {
		return (std::min(p1.first, p1.second) == std::min(p2.first, p2.second)
			&&
			std::max(p1.first, p1.second) == std::max(p2.first, p2.second));
	}
};

struct packet {
	std::unique_ptr<uint8_t[]> data;
	size_t size;
};
typedef std::deque<packet> packet_queue;

uint8_t generate_id() {
	if (!free_ids.empty()) {
		uint8_t id = free_ids.front();
		free_ids.pop();
		//std::cout << "returning free_id " << static_cast<int>(id) << "\n";
		return id;
	}
	if (next_id >= 254) {
		std::cerr << "ERROR max participants reached\n";
		return 255;//reserve id 255 for overflow error
	}
	else {
		uint8_t id = next_id++;
		//std::cout << "returning new id " << static_cast<int>(id) << "\n";
		return id;
	}
}
void release_id(uint8_t id) {
	//std::cout << "release id " << static_cast<int>(id) << "\n";
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
			//std::cout << "returning free_id " << static_cast<int>(id) << "\n";
			return id;
		}
		if (next_id >= 254) {
			std::cerr << "ERROR max participants reached\n";
			return 255;//reserve id 255 for overflow error
		}
		else {
			uint8_t id = next_id++;
			//std::cout << "returning new id " << static_cast<int>(id) << "\n";
			return id;
		}
	}
	void release_id(uint8_t id) {
		//std::cout << "releasing id " << static_cast<int>(id) << "\n";
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
		//participant_->set_vc_room_id(room_id_);
		participant_->start_read_vc_rb();
		//participant_->start_read_vc_rb();
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
		//std::cout << "try add vc participant\n";
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
			//std::cout << "vc room is 0\n";
			return; 
		}
		participants_[recv_vc_msg_->sender_id]->write_vc_msg_to_rb(recv_vc_msg_);
	}
	void send_message_to_playback(std::shared_ptr<voice_chat_message> recv_vc_msg_, uint8_t& sender_id) {
		auto self = shared_from_this();
		auto iter = participants_.begin();
		//std::cout << "recv_vc_msg->sender_id[" << static_cast<int>(recv_vc_msg_->sender_id) << "]\n";
		for (; iter != participants_.end(); ++iter) {
			auto msg_copy = std::make_shared<voice_chat_message>(*recv_vc_msg_);
			auto buffer = boost::asio::buffer(msg_copy->data(), msg_copy->length());
			size_t size = msg_copy->length();
			//std::cout << "write to client " << iter->second->name << "\n";
			//std::cout << "iter->second->id[" << static_cast<int>(iter->second->id) << "] msg_copy->sender_id["
				//<< static_cast<int>(sender_id) << "]\n";
			if (iter->second->id == sender_id && !iter->second->get_feedback_option()) { 
				//std::cout << "SKIP SEND SAME NAME\n";
				continue; }
			//std::cout << "do async send\n";
			if (iter->second->get_client_udp_endpoint() == nullptr) { continue; } //was crashing server. TODO find better solution
			udp_socket_->async_send_to(buffer, *iter->second->get_client_udp_endpoint(),
				[this, self, size, iter, msg_copy](boost::system::error_code ec, std::size_t bytes) {
					if (ec) {
						//std::cout << "write to client [" << iter->second->id << "][" << iter->second->name << "] fail\n";
					}
					else {
						//std::cout << "write to client [" << static_cast<int>(iter->second->id) << "][" << iter->second->name << "] @ port " 
							//<< " write to client size [" << size << "]\n\t" 
							//<< iter->second->get_client_udp_endpoint()->port() << " ip " << iter->second->get_client_udp_endpoint()->address() << " success\n";
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
		udp_socket_{ std::move(udp_socket) }, udp_endpoint_{ udp_endpoint }, io_context_{ io_context }, vc_hashmap{} {
	}
	boost::asio::io_context& io_context_;
	chat_message_queue recent_msgs_;
	std::unordered_map<uint8_t, chat_participant_ptr> participant_map;
	std::unordered_map<uint8_t, std::shared_ptr<voice_chat_room>> vc_rooms;
	std::unordered_map<uint8_t, std::string> tokens;

	void clean_vc_hash(uint8_t id) {
		auto iter = vc_hashmap.begin();
		for (; iter != vc_hashmap.end();) {
			if (iter->first.first == id) {
				auto check_second = participant_map.find(iter->first.second);
				if (check_second != participant_map.end()) {
					participant_map.at(iter->first.second)->get_vc_partner_ids()->erase(id);
				}
				iter->second.first = 0;
			}
			else if (iter->first.second == id) {
				auto  check_first = participant_map.find(iter->first.first);
				if (check_first != participant_map.end()) {
					participant_map.at(iter->first.first)->get_vc_partner_ids()->erase(id);
				}
				iter->second.second = 0;
			}
			bool remove = iter->second.first == 0 && iter->second.second == 0;
			if (remove) {
				iter = vc_hashmap.erase(iter);
			}
			else {
				++iter;
			}
		}
	}

	bool route_udp(uint8_t vc_room_id_, std::shared_ptr<voice_chat_message> recv_vc_msg_) {
		if (participant_map.find(recv_vc_msg_->sender_id) == participant_map.end()) { 
			std::cout << "sender id not found\n";
			return false; }
		participant_map.at(recv_vc_msg_->sender_id)->write_vc_msg_to_rb(recv_vc_msg_);
		return true;
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
	void send_room_full_notice() {
		chat_message m;
		std::string c = "no open rooms\n";
		std::memcpy(m.body(), c.c_str(), c.length());
		m.body_length(c.length());
		m.set_message_type(message_type::no_open_room);
		m.encode_header();
		deliver(m);
	}
	bool join(chat_participant_ptr participant) {
		std::cout << "participant w/ id " << static_cast<int>(participant->id) << " join room\n";
		if (duplicate_id(participant->id)) {
			return true;
		}		
		std::cout << "participant_map.size() = " << participant_map.size() << "\n";
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
		return true;
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
		//std::cout << "send_room_leave_notifications()\n";
		auto iter = participant_map.find(participant->id);
		if (iter == participant_map.end()) {
			//std::cout << "invalid participant\n";
			return; }
		auto p = participant_map.begin();
		for (; p != participant_map.end(); ++p) {
			chat_message msg;
			uint8_t sender_id = participant->id;
			msg.set_message_type(message_type::end_chat);
			std::memcpy(msg.body(), &sender_id, sizeof(sender_id));
			msg.body_length(sizeof(sender_id));
			msg.encode_header();
			//std::cout << "delivering leave notice of " << static_cast<int>(sender_id) <<
			//	" to " << p->second->name << "\n";
			p->second->deliver(msg);
		}
	}

	void leave_vc_room(chat_participant_ptr participant) {
		send_vc_leave_notifications(participant);
		uint8_t vc_room_id = participant->get_vc_room_id();
		std::cout << "leave room vc_room_id = " << static_cast<int>(vc_room_id) << "\n";
		uint8_t left_vc_room = 0;
		participant->set_feedback_option(false);
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
			//std::cout << "end search\n";
		}
	}
	void leave(chat_participant_ptr participant) {
		//MARKER1
		//participant->stop_read_vc_rb();
		//leave_vc_room(participant);//TODO get rid of this
		//leave any vc rooms first
		std::cout << "participant w/ id " << static_cast<int>(participant->id) << " leave chat room\n";
		//remove token
		if (!participant->id) { std::cout << "participant id is 0. not leaving room\n"; return; }
		std::cout << "clean_vc_hash\n";
		clean_vc_hash(participant->id);
		std::cout << "send_room_leave_notifications()\n";
		send_room_leave_notifications(participant);
		auto it = tokens.find(participant->id);
		std::cout << "search for token @ participant id " << static_cast<int>(participant->id) << "\n";
		if (it != tokens.end()) {
			tokens.erase(participant->id);
		}
		//leave chat room
		release_id(participant->id);
		participant->set_vc_enabled(0);
		participant_map.erase(participant->id);
		participant->id = 0;//3/26/26
		
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
		//std::cout << "serialize_participant()\n";
		uint8_t p_length_2 = 0;
		p_length_2 += sizeof(uint8_t);//id
		p_length_2 += sizeof(uint8_t);//vc
		std::string name = p->name;
		//std::cout << "\tname = " << name << "\n";
		uint8_t name_length = name.length();
		//std::cout << "\tname_length = " << static_cast<int>(name_length) << "\n";
		p_length_2 += sizeof(name_length);
		p_length_2 += name_length;

		std::memcpy(ptr, &p_length_2, sizeof(uint8_t));//length of participant data segment including id, vc_state, name_length and name
		std::memcpy(ptr + sizeof(uint8_t) * 1, &p->id, sizeof(uint8_t));//id
		uint8_t state = static_cast<uint8_t>(p->vc_state);
		std::memcpy(ptr + sizeof(uint8_t) * 2, &state, sizeof(uint8_t));//vc_state
		std::memcpy(ptr + sizeof(uint8_t) * 3, &name_length, sizeof(uint8_t));//name_length
		std::memcpy(ptr + sizeof(uint8_t) * 4, name.c_str(), name.length());//name
		std::string ptr_string = std::string(ptr, 4 + name.length());
		//std::cout << "\tptr[" << ptr_string << "]\n";
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
		//int i = 1;
		for (; it != participant_map.end(); ++it) {
			int i = serialize_participant(it->second, msg_test_2.body() + sizeof(uint8_t) * t_length_2);
			//std::cout << "\ti[" << i << "]\n";
			t_length_2 += i;
			//std::cout << "\tt_length_2[" << static_cast<int>(t_length_2) << "]\n";
		}
		msg_test_2.body_length(t_length_2);
		//std::string msg_test_2_body = std::string(msg_test_2.body(), msg_test_2.body_length());
		//std::cout << "msg_test_2.body[" << msg_test_2_body << "]\n";
		msg_test_2.encode_header();

		uint8_t pss = 0;
		char* ptr = msg_test_2.body();
		std::memcpy(&pss, ptr, sizeof(uint8_t));
		//std::cout << "ps[" << static_cast<int>(pss) << "]\n";
		ptr += sizeof(uint8_t);
		char* end = msg_test_2.body() + msg_test_2.body_length();
		//std::string mbody = std::string(msg_test_2.body(), msg_test_2.body_length());
		//std::cout << "\tm.body[" << mbody << "]\n";

		it =
			participant_map.begin();
		for (; it != participant_map.end(); ++it) {
			/*chat_participant p{};
			uint8_t len = 0;
			std::memcpy(&len, ptr, sizeof(uint8_t));
			std::cout << "\tlen[" << static_cast<int>(len) << "]\n";
			ptr += sizeof(uint8_t);
			size_t size = p.deserialize(ptr);
			std::cout << "delivering participant p:\n"
				<< "\tid[" << static_cast<int>(p.id) << "]"
				<< "\tname[" << p.name << "]\n";
			std::cout << "\tsize[" << size << "]\n";
			ptr += len;*/
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
	void set_vc_status(chat_message& m) {
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), 1);//sender id is always first
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + 1, 1);//followed by receiver id
		uint8_t sender_enable_vc = 0;
		std::memcpy(&sender_enable_vc, m.body() + 2, 1);
		auto iter_1 = participant_map.find(sender_id);
		auto iter_2 = participant_map.find(receiver_id);
		if (iter_1 == participant_map.end() || iter_2 == participant_map.end()) {
			std::cout << "invalid sender or receiver id on vc status check in\n";
			return;
		}
		if (sender_enable_vc) {
			participant_map.at(sender_id)->get_vc_partner_ids()->insert(receiver_id);
		}
		else {
			participant_map.at(sender_id)->get_vc_partner_ids()->erase(receiver_id);
		}
	}
	std::shared_ptr<boost::asio::ip::udp::endpoint> get_client_udp_endpoint(const uint8_t& client_id) {
		auto iter = participant_map.find(client_id);
		if (iter == participant_map.end()) { return nullptr; }
		return iter->second->get_client_udp_endpoint();
	}
	void print_client_vp_lists() {
		auto full_map_iter = participant_map.begin();
		for (; full_map_iter != participant_map.end(); ++full_map_iter) {
			std::string name = full_map_iter->second->name + "#" + std::to_string(static_cast<int>(full_map_iter->first));
			std::cout << "vc_partner_ids for " << name << "[\n";
			auto id_iter = full_map_iter->second->get_vc_partner_ids()->begin();
			for (; id_iter != full_map_iter->second->get_vc_partner_ids()->end(); ++id_iter) {
				std::cout << "\t[" << static_cast<int>(*id_iter) << "]\n";
			}
			std::cout << "]\n";
		}
	}
	void send_server_udp_port(uint8_t& client_id, uint8_t& partner_id) {
		chat_message msg;
		msg.set_message_type(message_type::send_udp_port);
		std::memcpy(msg.body(), &client_id, 1);
		std::memcpy(msg.body() + 1, &partner_id, 1);
		std::memcpy(msg.body() + 2, &udp_port, sizeof(udp_port));
		msg.body_length(2 + sizeof(udp_port));
		msg.encode_header();
		std::cout << "send_server_udp_port() - \n\tclient_id = "
			<< static_cast<int>(client_id)
			<< "\n\tudp_port = "
			<< static_cast<int>(udp_port) << "\n";
		if (participant_map.find(client_id) != participant_map.end()) {
			participant_map.at(client_id)->deliver(msg);
		}
		else {
			std::cout << "client_id not found\n";
			return;
		}
	}
	void send_partner_id(uint8_t& client_id, uint8_t& partner_id) {
		chat_message msg;
		msg.set_message_type(message_type::vc_partner_update);
		std::memcpy(msg.body(), &client_id, 1);
		std::memcpy(msg.body() + 1, &partner_id, 1);
		msg.body_length(2);
		msg.encode_header();
		if (participant_map.find(client_id) != participant_map.end()) {
			participant_map.at(client_id)->deliver(msg);
		}
		else {
			std::cout << "client_id not found\n";
			return;
		}
	}
	void stop_client_vc(uint8_t& sender_id) {
		chat_message msg;
		msg.set_message_type(message_type::end_vc);
		msg.body_length(0);
		if (participant_map.find(sender_id) != participant_map.end()) {
			participant_map.at(sender_id)->deliver(msg);
			participant_map.at(sender_id)->stop_read_vc_rb();
		}
	}
	void update_vc_status(chat_message& m) {
		std::cout << "update_vc_status()\n";
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), 1);
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + 1, 1);
		uint8_t sender_enable_vc = 0;
		std::memcpy(&sender_enable_vc, m.body() + 2, 1);
		auto iter_a = participant_map.find(sender_id);
		auto iter_b = participant_map.find(receiver_id);
		std::cout << "\tsender_id[" << static_cast<int>(sender_id) << "]\n"
			<< "\treceiver_id[" << static_cast<int>(receiver_id) << "]\n"
			<< "\tsender_enable_vc[" << static_cast<int>(sender_enable_vc) << "]\n";
		if (iter_a == participant_map.end()) {
			std::cout << "\tinvalid sender\n";
			return;
		}
		uint8_t turn_on_client_vc = 0;
		uint8_t turn_off_client_vc = 0;
		if (sender_enable_vc) {
			if (participant_map.at(sender_id)->get_vc_partner_ids()->size() == 0) {
				turn_on_client_vc = 1;
			}	
			if (iter_b != participant_map.end()) {
				std::cout << "\tinsert receiver_id [" << static_cast<int>(receiver_id) << "]"
					<< "\n\tinto participant_map.at(" << static_cast<int>(sender_id) << ")\n";
				
				participant_map.at(sender_id)->get_vc_partner_ids()->insert(receiver_id);
				std::cout << "get_vc_partner_ids().size() = " << participant_map.at(sender_id)->get_vc_partner_ids()->size() << "\n";

			}
		}	
		else if (!sender_enable_vc) {
			if (participant_map.at(sender_id)->get_vc_partner_ids()->find(receiver_id) !=
				participant_map.at(sender_id)->get_vc_partner_ids()->end()) {
				participant_map.at(sender_id)->get_vc_partner_ids()->erase(receiver_id);
			}
			if (participant_map.at(sender_id)->get_vc_partner_ids()->size() == 0) {
				turn_off_client_vc = 1;
			}
		}
		if (turn_on_client_vc) {
			std::cout << "\tturn on client vc\n";
			participant_map.at(sender_id)->start_read_vc_rb();
			send_server_udp_port(sender_id, receiver_id);
		}
		if (turn_off_client_vc) {
			std::cout << "\tturn off client vc\n";
			//stop_client_vc(sender_id);
			participant_map.at(sender_id)->stop_read_vc_rb();
		}
	}
	void update_vc_status_test(chat_message& m) {
		print_client_vp_lists();
		std::cout << "update_vc_status_test()\n";
		uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), 1);
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + 1, 1);
		uint8_t sender_enable_vc = 0;
		std::memcpy(&sender_enable_vc, m.body() + 2, 1);
		auto iter_a = participant_map.find(sender_id);
		auto iter_b = participant_map.find(receiver_id);
		std::cout << "\tsender_id[" << static_cast<int>(sender_id) << "]\n"
			<< "\treceiver_id[" << static_cast<int>(receiver_id) << "]\n"
			<< "\tsender_enable_vc[" << static_cast<int>(sender_enable_vc) << "]\n";
		if (iter_a == participant_map.end()) {
			std::cout << "\tinvalid sender\n";
			return;
		}
		if (iter_b == participant_map.end()) {
			std::cout << "\tinvalid receiver\n";
			return;
		}
		uint8_t turn_on_client_vc = 0;
		uint8_t turn_on_client_a_vc = 0;
		uint8_t turn_on_client_b_vc = 0;

		uint8_t client_a_id = std::min(sender_id, receiver_id);
		uint8_t client_b_id = std::max(sender_id, receiver_id);

		std::pair<uint8_t, uint8_t> p = std::make_pair(client_a_id, client_b_id);
		auto hash_iter = vc_hashmap.find(p);
		if (hash_iter != vc_hashmap.end()) {
			if (hash_iter->first.first == sender_id) {
				vc_hashmap.at(p).first = sender_enable_vc;
			}
			else {
				vc_hashmap.at(p).second = sender_enable_vc;
			}
		}
		else {
			uint8_t self_send = sender_id == receiver_id ? 1 : 0;
			if (p.first == sender_id) {
				vc_hashmap.insert(std::make_pair(p, std::make_pair(sender_enable_vc, self_send)));
				std::cout << "p.first == sender_id\n\t[sender_enable_vc = " << static_cast<int>(sender_enable_vc) << "]"
					<< "\n\t[self_send = " << static_cast<int>(self_send) << "]\n";
			}			
			else {
				vc_hashmap.insert(std::make_pair(p, std::make_pair(self_send, sender_enable_vc)));
				vc_hashmap.insert(std::make_pair(p, std::make_pair(sender_enable_vc, self_send)));
				std::cout << "NOT p.first == sender_id\n\t[sender_enable_vc = " << static_cast<int>(sender_enable_vc) << "]"
					<< "\n\t[self_send = " << static_cast<int>(self_send) << "]\n";
			}
		}
		std::cout << "print vc_hashmap:\n";
		auto debug_hashmap_iter = vc_hashmap.begin();
		for (; debug_hashmap_iter != vc_hashmap.end(); ++debug_hashmap_iter) {
			std::cout << "\t["
				<< static_cast<int>(debug_hashmap_iter->first.first) << ", "
				<< static_cast<int>(debug_hashmap_iter->first.second) << "]["
				<< static_cast<int>(debug_hashmap_iter->second.first) << ", "
				<< static_cast<int>(debug_hashmap_iter->second.second) << "]\n";
		}
		turn_on_client_vc = (vc_hashmap.at(p).first & vc_hashmap.at(p).second);

		if (turn_on_client_vc == 1) {
			std::cout << "turn on client vc == 1\n";
			participant_map.at(client_a_id)->get_vc_partner_ids()->insert(client_b_id);
			participant_map.at(client_b_id)->get_vc_partner_ids()->insert(client_a_id);
			if (participant_map.at(client_a_id)->get_vc_enabled() == 0) {
				std::cout << "turn on client_a [" << static_cast<int>(client_a_id) << "]\n";
				participant_map.at(client_a_id)->start_read_vc_rb();
				participant_map.at(client_a_id)->set_vc_enabled(1);
				send_server_udp_port(client_a_id, client_b_id);
			}
			else {
				std::cout << "send_partner_id(client_a_id, client_b_id);\n";
				send_partner_id(client_a_id, client_b_id);
			}
			if (client_a_id != client_b_id) {
				if (participant_map.at(client_b_id)->get_vc_enabled() == 0) {
					std::cout << "turn on client_b [" << static_cast<int>(client_b_id) << "]\n";
					participant_map.at(client_b_id)->start_read_vc_rb();
					participant_map.at(client_b_id)->set_vc_enabled(1);
					send_server_udp_port(client_b_id, client_a_id);
				}
				else {
					std::cout << "send_partner_id(client_b_id, client_a_id);\n";
					send_partner_id(client_b_id, client_a_id);
				}
			}
		}
		else if (turn_on_client_vc == 0) {
			auto iter_b_in_a = participant_map.at(client_a_id)->get_vc_partner_ids()->find(client_b_id);
			if (iter_b_in_a != participant_map.at(client_a_id)->get_vc_partner_ids()->end()) {
				participant_map.at(client_a_id)->get_vc_partner_ids()->erase(client_b_id);
				std::cout << "turn off client_b [" << static_cast<int>(client_b_id) << "] in client_a[" 
					<< static_cast<int>(client_a_id) <<  "] vc\n";
			}
			auto iter_a_in_b = participant_map.at(client_b_id)->get_vc_partner_ids()->find(client_a_id);
			if (iter_a_in_b != participant_map.at(client_b_id)->get_vc_partner_ids()->end()) {
				participant_map.at(client_b_id)->get_vc_partner_ids()->erase(client_a_id);
				std::cout << "turn off client_a [" << static_cast<int>(client_a_id) << "] in client_b["
					<< static_cast<int>(client_b_id) << "] vc\n";
			}
			std::cout << "\tturn off client vc\n";
			//stop_client_vc(sender_id);
			if (participant_map.at(client_a_id)->get_vc_partner_ids()->size() == 0 && participant_map.at(client_a_id)->get_vc_enabled() == 1) {
				participant_map.at(client_a_id)->stop_read_vc_rb();
				participant_map.at(client_a_id)->set_vc_enabled(0);
			}
			if (participant_map.at(client_b_id)->get_vc_partner_ids()->size() == 0 && participant_map.at(client_b_id)->get_vc_enabled() == 1) {
				participant_map.at(client_b_id)->stop_read_vc_rb();
				participant_map.at(client_b_id)->set_vc_enabled(0);
			}
		}

		auto full_map_iter = participant_map.begin();
		for (; full_map_iter != participant_map.end(); ++full_map_iter) {
			std::string name = full_map_iter->second->name + "#" + std::to_string(static_cast<int>(full_map_iter->first));
			std::cout << "vc_partner_ids for " << name << "[\n";
			auto id_iter = full_map_iter->second->get_vc_partner_ids()->begin();
			for (; id_iter != full_map_iter->second->get_vc_partner_ids()->end(); ++id_iter) {
				std::cout << "\t[" << static_cast<int>(*id_iter) << "]\n";
			}
			std::cout << "]\n";
		}
	}
	void get_receiver_vc_status(chat_message& m) {
		//don't need to do this shit
		/*uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), 1);//sender id is always first
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + 1, 1);//followed by receiver id
		uint8_t sender_enable_vc = 0;
		std::memcpy(&sender_enable_vc, m.body() + 2, 1);//check if the receiver has voice enabled for the sender

		chat_message msg;
		msg.msg_type = message_type::vc_status_check;
		msg.body_length(sizeof(sender_id) + sizeof(receiver_id) + sizeof(sender_enable_vc));
		std::memcpy(msg.body(), &sender_id, 1);
		std::memcpy(msg.body() + 1, &receiver_id, 1);
		std::memcpy(msg.body() + 2, &sender_enable_vc, 1);
		participant_map.at(receiver_id)->deliver(msg);*/
	}
	void handle_vc_status_response(chat_message& m) {
		//don't need to do all this shit
		/*uint8_t sender_id = 0;
		std::memcpy(&sender_id, m.body(), 1);//sender id is always first
		uint8_t receiver_id = 0;
		std::memcpy(&receiver_id, m.body() + 1, 1);//followed by receiver id
		uint8_t sender_enable_vc = 0;
		std::memcpy(&sender_enable_vc, m.body() + 2, 1);//does sender have voice enabled for receiver?
		uint8_t receiver_enable_vc = 0;
		std::memcpy(&receiver_enable_vc, m.body() + 3, 1);//does receiver have voice enabled for sender?		
		if (sender_enable_vc & receiver_enable_vc) {
			//start vc
			auto iter_1 = participant_map.find(sender_id);
			auto iter_2 = participant_map.find(receiver_id);
			if (iter_1 != participant_map.end() && iter_2 != participant_map.end()) {
				auto iter_11 = participant_map.at(sender_id)->get_vc_partner_ids().find(receiver_id);
				auto iter_22 = participant_map.at(receiver_id)->get_vc_partner_ids().find(sender_id);
				if (iter_11 != participant_map.at(sender_id)->get_vc_partner_ids().end()) {
					participant_map.at(sender_id)->get_vc_partner_ids().insert(receiver_id);
					//TODO open vc for both participants
				}
				if (iter_22 != participant_map.at(receiver_id)->get_vc_partner_ids().end()) {
					participant_map.at(receiver_id)->get_vc_partner_ids().insert(sender_id);
					//TODO open vc for both participants
				}

			}
		}
		else {
			//stop vc
			auto iter_1 = participant_map.find(sender_id);
			if (iter_1 != participant_map.end()) {
				auto iter_2 = participant_map.at(sender_id)->get_vc_partner_ids().find(receiver_id);
				if (iter_2 != participant_map.at(sender_id)->get_vc_partner_ids().end()) {
					participant_map.at(sender_id)->get_vc_partner_ids().erase(receiver_id);
					//TODO close vc for both participants
				}
			}

		}*/

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
		//do a loop here and instead of copying in whole array, copy in count amount
		for (int i = 1; i <= count; ++i) {
			std::memcpy(msg.body() + sizeof(count), &ids[i - 1], 1);
		}
		//std::memcpy(msg.body() + sizeof(count), ids, sizeof(ids));
		std::memcpy(msg.body() + sizeof(count) + count, &vc_room_id, sizeof(vc_room_id));
		std::memcpy(msg.body() + sizeof(count) + count + sizeof(vc_room_id), &udp_port, sizeof(udp_port));//2bytes
		std::cout << "udp_port[" << static_cast<int>(udp_port) << "]\n";
		msg.body_length(sizeof(count) + count + sizeof(vc_room_id) + sizeof(udp_port));
		msg.encode_header();
		std::string header = std::string(msg.data(), chat_message::header_length);
		//std::string body = std::string(msg.body(), msg.body_length());
		uint8_t vcroomid = 0;
		std::memcpy(&vcroomid, msg.body() + sizeof(count) + count, sizeof(vcroomid));
		std::cout << "vcroomid[" << static_cast<int>(vcroomid) << "]\n";
		std::cout << "sizeof(ids)[" << sizeof(ids) << "]\n";
		std::cout << "accept_vc_request count[" << static_cast<int>(count) << "] header[" << header << "] body[" << msg.body() << "]\n";
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
		msg.body_length(sizeof(sender_id) + sizeof(vc_room_id) + sizeof(udp_port));
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
	std::unordered_map<std::pair<uint8_t, uint8_t>, std::pair<uint8_t, uint8_t>, pair_hash, pair_equality> vc_hashmap;
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
	std::unordered_set<uint8_t> vc_partner_ids;
	std::unordered_set<uint8_t> vc_requestor_ids;
	uint8_t vc_enabled;

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
		version_validated(false),
		io_context{io_context},
		vc_room_id(0),
		vc_partner_ids{},
		vc_requestor_ids{},
		vc_enabled{0},
		heartbeat_timer{std::make_shared<boost::asio::steady_timer>(io_context)},
		disconnect_timer{std::make_shared<boost::asio::steady_timer>(io_context)}
	{
	}
	void update_vc_partner_id(uint8_t receiver_id, std::pair<uint8_t,uint8_t> p) {
	}
	std::unordered_set<uint8_t>* get_vc_partner_ids() override {
		return &vc_partner_ids;
	}
	std::unordered_set<uint8_t>* get_vc_requestor_ids() override {
		return &vc_requestor_ids;
	}
	bool get_feedback_option() override {
		return feedback;
	}
	uint8_t get_vc_enabled() override {
		return vc_enabled;
	}
	void set_vc_enabled(uint8_t val) override {
		vc_enabled = val;
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
		version_validated = false;
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
		ma_result result = ma_rb_init(buffer_size, NULL, NULL, &vc_rb);
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
		//std::cout << "write_vc_msg_to_rb()\n";
		if (vc_partner_ids.size() == 0) { 
			//std::cout << "vc_partner_ids.size() = " << vc_partner_ids.size() << "\n";
			return; }
		void* pwrite_void = nullptr;
		size_t size = recv_vc_msg_->body_length();
		size_t requested = size;
		//std::cout << "size = " << size << "\n";
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
		//std::cout << "encoding:"
			///<< "\n\tsession_token[" << session_token << "]"
			//<< "\n\tsize[" << static_cast<int>(size) << "]"
			//<< "\n\tvc_room_id[" << static_cast<int>(vc_room_id) << "]"
			//<< "\n\tid[" << static_cast<int>(id) << "]"
			//<< "\n\ttest[" << static_cast<int>(test) << "]\n";
		m->encode_header(session_token, size, vc_room_id, id);
		std::memcpy(m->body(), in, size);
		//std::cout << "m->sender_id[" << static_cast<int>(m->sender_id) << "]\n";
		//std::cout << "send_message_to_playback()\n";		
		send_message_to_playback(m, id);	
		return true;
	}
	void send_message_to_playback(std::shared_ptr<voice_chat_message> recv_vc_msg_, uint8_t& sender_id) {
		auto self = shared_from_this();
		auto iter = vc_partner_ids.begin();
		//std::cout << "recv_vc_msg->sender_id[" << static_cast<int>(recv_vc_msg_->sender_id) << "]\n";
		for (; iter != vc_partner_ids.end(); ++iter) {
			auto msg_copy = std::make_shared<voice_chat_message>(*recv_vc_msg_);
			auto buffer = boost::asio::buffer(msg_copy->data(), msg_copy->length());
			size_t size = msg_copy->length();
			//std::cout << "do async send\n";
			
			std::shared_ptr<boost::asio::ip::udp::endpoint> ep = room_->get_client_udp_endpoint(*iter);
			if (ep == nullptr) { continue; } //was crashing server. TODO find better solution
			
			udp_socket_->async_send_to(buffer, *ep,
				[self, size, iter, msg_copy, ep](boost::system::error_code ec, std::size_t bytes) {
					if (ec) {
						std::cout << "write to client [" << *iter << "] fail: " << ec.message() << "\n";
					}
					else {
						//std::cout << "write from client[" << static_cast<int>(id) << "] @ port " << get_client_udp_endpoint()->port() << "/ip " << get_client_udp_endpoint()->address()
						//	<< " to client[" << static_cast<int>(*iter) << "] @ port "  << ep->port()
							//<< " write to client size [" << size << "]\n\t" 
							//<< ep->port() << " ip " << ep->address() << " success\n";
					}	
				}
			);
			//std::cout << "async send complete\n";
		}
		//don't want to commit read for every person, but need to only commit after all sends have completed??
		//std::cout << "get sender\n";
		
		//std::cout << "at id " << static_cast<int>(sender_id) << "\n";
		//std::cout << "commit read of size " << recv_vc_msg_->body_length() << "\n";
		//std::cout << "send to " << sender->name << "\n";
		commit_read_rb(recv_vc_msg_->body_length());
	}

	void start_read_vc_rb() override {
		running_vc = true;
		init_rb();
		auto self = shared_from_this();
		boost::asio::post(io_context, [self] { self->read_vc_rb(); });
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
			boost::asio::post(io_context, [self] { self->read_vc_rb(); });
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
		boost::asio::post(io_context, [self] { self->read_vc_rb(); });
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
	void set_feedback_option(bool val) override {
		feedback = val;
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
	bool verify_version(chat_message& msg) {
		if(msg.body_length() != current_version_length){
			std::cout << "bad version length: " << msg.body_length() << "\n";
			std::string v = std::string(msg.body(), msg.body_length());
			std::cout << "msg body() = " << v << "\n";
			return false;
		}
		std::string version(msg.body(), msg.body_length());
		std::cout << "client_version = " << version << "\n";
		if (version != current_version) {
			std::cout << "bad version\n";
			chat_message m;
			std::string c = "client out of date\n";
			std::memcpy(m.body(), c.c_str(), c.length());
			m.body_length(c.length());
			m.set_message_type(message_type::version_check);
			m.encode_header();
			deliver(m);
			return false;
		}
		return true;
	}
	void store_client_udp_port(chat_message& msg) {
		std::cout << "store_client_udp_port!\n";
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
		//std::cout << "test\n";
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
	void do_shutdown(chat_session& obj, std::shared_ptr<chat_session> self) {
		obj.ssl_socket_.async_shutdown(
			[self](const boost::system::error_code& ec) {
				auto& obj = *self;
				boost::system::error_code ignored;
				obj.ssl_socket_.lowest_layer().close(ignored);
			});
	}
	void do_handshake() {
		//std::cout << "do_handshake()\n";
		auto self(shared_from_this());
		//std::cout << "start handshake\n";
		ssl_socket_.async_handshake(boost::asio::ssl::stream_base::server,
			[self](const boost::system::error_code& ec) {
				auto& obj = *self;
				if (!ec) {
					std::cout << "handshake success\n";
					obj.wait_for_ready();
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
			[self](boost::system::error_code ec, std::size_t)
			{
				auto& obj = *self;
				if (!ec && obj.read_msg_.decode_header()) {
					//std::string header = std::string(read_msg_.data(), chat_message::header_length);
					//std::cout << "read_msg_header[" << header << "]\n";					
					obj.do_read_body_ssl();
				}
				else {
					std::cerr << "read error ssl: " << ec.message() << "\n";
					obj.room_->leave(obj.shared_from_this());
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
					std::string header = std::string(read_msg_.data(), chat_message::header_length);
					std::cout << "read_msg_header[" << header << "]\n";					
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
			[self](boost::system::error_code ec, std::size_t)
			{
				auto& obj = *self;
				if (!ec) {
					//std::string header = std::string(read_msg_.data(), chat_message::header_length);
					//std::string body = std::string(read_msg_.body(), read_msg_.body_length());
					//std::cout << "read msg header[" << header << "] body [" << body << "]\n";
					//validate client is authorized!! TODO
					if (!obj.authenticated) {
						if (!obj.version_validated) {
							obj.version_validated = obj.verify_version(obj.read_msg_);
							if (!obj.version_validated) {
								/*obj.ssl_socket_.async_shutdown(
									[self](const boost::system::error_code& ec) {
										auto& obj = *self;
										boost::system::error_code ignored;
										obj.ssl_socket_.lowest_layer().close(ignored);
									}
								);*/
								self->do_shutdown(obj, self);
							}
						}
						if (obj.verify_authorization_response(obj.read_msg_)) {
							chat_message m;
							m.body_length(obj.session_token.length());//16 bytes/chars
							m.set_message_type(message_type::authentication_approve);
							std::memcpy(m.body(), obj.session_token.c_str(), m.body_length());
							std::cout << "verify auth encode header\n";
							m.encode_header();
							std::cout << "header encoded\n";
							obj.deliver(m);
							obj.authenticated = true;
							obj.start_heartbeat();
							obj.start_disconnect_timer();
						}
						else {
							obj.send_authentication_request();
							obj.authenticated = false;
							//room_->leave(shared_from_this());
						}
						obj.do_read_header_ssl();
						return;
					}
					switch (obj.read_msg_.msg_type) {
						/*case(message_type::ready_notification): {
							if (state_ == session_state::wait) {
								send_authentication_request();
							}
							break;
						}*/
					case(message_type::no_open_room): {
						obj.do_shutdown(obj, self);
					}
					case(message_type::start_room_request): {
						if (obj.room_->participant_map.size() >= max_participants) {
							std::cout << "room full! name = " << obj.name << "\n";
							obj.room_->send_room_full_notice();
							obj.stop_heartbeat();
						}
						if (obj.authenticated) {
							if (obj.room_->check_room_full()) {
								obj.reject();
							}
							else {
								obj.start();
							}
						}
						break;
					}
					case(message_type::name_change_request): {
						obj.state_ = session_state::awaiting_name;
						//name is preceded by uint8_t id
						std::string name(obj.read_msg_.body() + sizeof(uint8_t), obj.read_msg_.body_length() - sizeof(uint8_t));
						obj.room_->leave(obj.shared_from_this());
						//std::cout << "new name = " << name << "\n";
						if (!name.empty()) {
							uint8_t id = 0;
							std::memcpy(&id, obj.read_msg_.body(), sizeof(uint8_t));
							obj.change_name(name);
							//std::cout << "name changed\n";
							//this->id = id;
							obj.room_->join(obj.shared_from_this());
							obj.room_->update_client_participants();
						}
						break;
					}
					case(message_type::chat): {
						std::string full_msg = obj.name + ": ";
						full_msg.append(obj.read_msg_.body(), obj.read_msg_.body_length());
						obj.read_msg_.body_length(full_msg.length());
						memcpy(obj.read_msg_.body(), full_msg.c_str(), obj.read_msg_.body_length());
						obj.read_msg_.encode_header();
						obj.room_->deliver(obj.read_msg_);
						break;
					}
					case(message_type::send_vc_request): {
						obj.room_->send_vc_request(obj.read_msg_);
						break;
					}
					case(message_type::reject_vc_request): {
						obj.room_->reject_vc_request(obj.read_msg_);
						break;
					}
					case(message_type::accept_vc_request): {
						obj.room_->accept_vc_request(obj.read_msg_);
						break;
					}
					case(message_type::send_udp_port): {
						//std::cout << "got send_udp_port\n";
						obj.store_client_udp_port(obj.read_msg_);//TODO code review this
						break;
					}
					case(message_type::mic_test): {
						obj.room_->accept_mic_check_request(obj.read_msg_);
						obj.set_feedback_option(true);
						break;
					}
					case(message_type::end_vc): {
						//room_->send_vc_leave_notification(shared_from_this());
						//room_->send_vc_leave_notifications(shared_from_this());
						obj.room_->leave_vc_room(obj.shared_from_this());
						break;
					}
					case(message_type::vc_status_check): {
						obj.room_->update_vc_status_test(obj.read_msg_);
						//room_->get_receiver_vc_status(read_msg_);
						//don't do a status check. 
						//check if enabling or disabling vc
						//if enabling, just add the receiver to the sender vc_partner_ids hash
						//then check the receiver's vc_partner_ids hash to see if he has sender
						//if both have, then start vc
						//if disabling, check if receiver has sender in vc_partner_ids
						//yes? then remove
						//server will send to client his hash of vc_partners, and client will then
						//copy that to his own hash and send vc messages to them, which will be routed by server
					}
					case(message_type::vc_status_response): {
						obj.room_->handle_vc_status_response(obj.read_msg_);
						break;
					}
					case(message_type::heartbeat): {
						obj.reset_disconnect(obj.read_msg_);
						break;
					}
					case(message_type::leave): {
						obj.room_->leave(obj.shared_from_this());
					}
					}
					obj.do_read_header_ssl();
				}
				else {
					std::cerr << "Write error: " << ec.message() << "\n";
					obj.room_->leave(obj.shared_from_this());
					obj.authenticated = false;
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
			[self, msg](boost::system::error_code ec, std::size_t)
			{
				auto& obj = *self;
				if (!ec) {
					std::string header = std::string(msg.data(), chat_message::header_length);
					std::string body = std::string(obj.write_msgs_.front().body(), obj.write_msgs_.front().body_length());
					//std::cout << "write msg header[" << header << "] body [" << body << "]\n";
						//std::cout << "!ec\n";
					obj.write_msgs_.pop_front();
					if (!obj.write_msgs_.empty()) {
						obj.do_write_ssl();
					}
				}
				else {
					std::cerr << "Write error: " << ec.message() << "\n";
					obj.room_->leave(obj.shared_from_this());
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
	void heartbeat_ping(const boost::system::error_code& e,
		std::shared_ptr<boost::asio::steady_timer> t) {
		if (e == boost::asio::error::operation_aborted) { return; }
		t->expires_after(boost::asio::chrono::seconds(20));
	
		chat_message m;
		m.set_message_type(message_type::heartbeat);
		std::memcpy(m.body(), &id, 1);
		m.body_length(sizeof(id));
		m.encode_header();
		deliver(m);
		auto self = shared_from_this();
		t->async_wait([self, t](const boost::system::error_code& ec) { self->heartbeat_ping(ec, t); });
	}
	void start_heartbeat() {
		auto self = shared_from_this();
		auto timer = heartbeat_timer;
		heartbeat_timer->async_wait([self, timer](const boost::system::error_code& ec) {
			self->heartbeat_ping(ec, timer); 
			});
	}
	void stop_heartbeat() {
		heartbeat_timer->cancel();
	}
	void reset_disconnect(chat_message& m) {
		disconnect_timer->expires_after(std::chrono::seconds(60));
	}
	void disconnect(const boost::system::error_code& e,
		std::shared_ptr<boost::asio::steady_timer> t) {
		if (e == boost::asio::error::operation_aborted) { return; }
		std::cout << "disconnect trigger\n";
		auto self = shared_from_this();
		auto& obj = *self;
		do_shutdown(obj, self);
		room_->leave(shared_from_this());
	}
	void start_disconnect_timer() {
		disconnect_timer->expires_after(std::chrono::seconds(60));
		auto self = shared_from_this();
		auto timer = disconnect_timer;
		disconnect_timer->async_wait([self, timer](const boost::system::error_code& ec) {
			self->disconnect(ec, timer);
		});
	}
	boost::asio::ssl::stream<tcp::socket> ssl_socket_;
	tcp::socket socket_;
	std::shared_ptr<chat_room> room_;
	chat_message read_msg_;
	chat_message_queue write_msgs_;
	session_state state_ = session_state::wait;
	bool version_validated = false;
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
	std::shared_ptr<boost::asio::steady_timer> heartbeat_timer;
	std::shared_ptr<boost::asio::steady_timer> disconnect_timer;
	
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
		do_receive();
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
				std::cout << "ssl tcp connection from [" << client_ip << "] on port[" << client_port << "]\n";
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
				//udp_remote_endpoint_,
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
	void handle_receive(/*udp::endpoint& remote_endpoint,*/ std::shared_ptr<voice_chat_message> recv_vc_msg_, const boost::system::error_code& error,
		std::size_t) {
		if (!error) {
			if (recv_vc_msg_->decode_header()                                                                   ) {
				std::string client_ip = udp_remote_endpoint_.address().to_string();
				unsigned short client_port = udp_remote_endpoint_.port();
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
		udp_start_receive();
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