#ifndef CHAT_MESSAGE_H
#define CHAT_MESSAGE_H	
//#include <cstdio>
//#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <string>

enum class message_type : uint8_t {
	chat = 1,
	name_challenge = 2,
	name_response = 3,
	name_approve = 4,
	name_change_request = 5,
	particpants_request = 6,
	ping = 7,
	authentication_request = 8,
	authentication_response = 9,
	authentication_approve = 10,
	authentication_reject = 11,
	start_room_request = 12,
	ready_notification = 13,
	send_vc_request = 14,
	recieve_vc_request = 15,
	reject_vc_request = 16,
	accept_vc_request = 17,
	send_vc = 18,
	recieve_vc = 19,
	reject_join = 20,
	send_id = 21,
	send_udp_port = 22,
	start_vc= 23,
	mic_test = 24,
	end_vc = 25,
	end_chat = 26,
	vc_status_check = 27,
	vc_status_response = 28,
	vc_partner_update = 29,
	bad_message = 30
};

class chat_message {
public:
	static constexpr std::size_t msg_type_length = 2;
	static constexpr std::size_t msg_length = 4;
	static constexpr std::size_t header_length = 6;
	static constexpr std::size_t max_body_length = 512;
	message_type msg_type;
	chat_message() : body_length_(0) {}
	const char* data() const {
		return data_;
	}
	char* data() {
		return data_;
	}
	std::size_t length() const {
		return header_length + body_length_;
	}
	const char* body() const {
		return data_ + header_length;
	}
	char* body() {
		return data_ + header_length;
	}
	std::size_t body_length() const {
		return body_length_;
	}
	void body_length(std::size_t new_length) {
		body_length_ = new_length;
		if (body_length_ > max_body_length)
			body_length_ = max_body_length;
	}
	void set_message_type(message_type m) {
		msg_type = m;
	}
	bool decode_header() {
		//std::cout << "decode_header\n";
		char header[header_length + 1] = "";// +1 for null terminator
		std::strncat(header, data_, header_length);
		//std::cout << "decode_header_2[" << std::string(header, 2) << "]\n";
		//std::cout << "decode_header_4[" << std::string(header + msg_type_length, 4) << "]\n";
		try {
			msg_type = static_cast<message_type>(std::stoi(std::string(header, msg_type_length)));
		}
		catch(const std::invalid_argument& e){
			std::cout << "=( bad header: " << header << "\n";
			//std::cout << "data: " << data_ << "\n";
			msg_type = message_type::bad_message;
			return false;
		}
		try {
			body_length_ = std::stoi(std::string(header + msg_type_length, 4));
		}
		catch (const std::invalid_argument& e) {
		std::cout << " std::stoi(std::string(header + 2, 4))\n";
			std::string text = "bad";
			body_length_ = 3;
			std::memcpy(body(), text.c_str(), body_length());
			return false;
		}
		if (msg_type < message_type(message_type::chat) || msg_type > message_type(message_type::bad_message)) {
			std::cout << "bad msg_type: " << std::to_string(static_cast<int>(msg_type)) << "\n";
			return false;
		}
		if (body_length_ > max_body_length) {
			body_length_ = 0;
			return false;
		}
		std::cout << "header decoded success\n";
		return true;
	}
	void encode_header() {
		char header[header_length + 1] = "";
		//std::sprintf(header, "%02d%04d",static_cast<int>(msg_type), static_cast<int>(body_length_));
		header[0] = '0' + (static_cast<int>(msg_type) / 10);
		header[1] = '0' + (static_cast<int>(msg_type) % 10);
		int len = static_cast<int>(body_length_);
		header[2] = '0' + (len / 1000) % 10;
		header[3] = '0' + (len / 100) % 10;
		header[4] = '0' + (len / 10) % 10;
		header[5] = '0' + (len % 10);

		//std::cout << "encode_header[" << header << "]\n";
		std::memcpy(data_, header, header_length);
	}
private:
	char data_[header_length + max_body_length];
	std::size_t body_length_;
};
#endif
