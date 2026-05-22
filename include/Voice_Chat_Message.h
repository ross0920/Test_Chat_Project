#ifndef VOICE_CHAT_MESSAGE_H
#define VOICE_CHAT_MESSAGE_H
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "miniaudio.h"

class voice_chat_message {
public:
	// max playback size = 3840;
	// buffer size = 19200;
	static constexpr std::size_t buffer_size = 4000;
	static constexpr std::size_t max_body_length = buffer_size;
	static constexpr std::size_t token_length = 16;
	static constexpr std::size_t header_length = 1 + token_length + 1 + 1 + 2;
	static constexpr std::size_t max_length = max_body_length + header_length;
	uint8_t token_len;//1
	std::string token;//16
	uint8_t room_id;//1
	uint8_t sender_id;//1
	uint16_t body_length_;//2

	const uint8_t* data() const {
		return data_;
	}
	uint8_t* data() {
		return data_;
	}
	std::size_t length() const {
		return header_length + body_length_;
	}
	const uint8_t* body() const {
		return data_ + header_length;
	}
	uint8_t* body() {
		return data_ + header_length;
	}
	uint16_t body_length() const {
		return body_length_;
	}
	void body_length(uint16_t new_length) {
		body_length_ = new_length;
		if (body_length_ > max_body_length)
			body_length_ = max_body_length;
	}
	void encode_header(std::string token_, uint16_t new_body_length_, uint8_t room_id_, uint8_t sender_id_) {
		std::cout << "ENCODE THIS THING\n";
		token_len = token_length;
		//std::cout << "assigning token\n";
		token = token_;
		//std::cout << "cast body length\n";
		body_length_ = static_cast<uint16_t>(new_body_length_);
		//std::cout << "set ptr\n";
		uint8_t* ptr = data_;
		//std::cout << "write token_len: [" << static_cast<int>(token_len) << "]\n";
		std::memcpy(ptr, &token_len, sizeof(token_len));
		ptr += sizeof(token_len);
		std::memcpy(ptr, token.c_str(), token_length);
		ptr += token_length;
		//std::cout << "write token: [" << token << "]\n";
		std::memcpy(ptr, &room_id_, sizeof(room_id_));
		//std::cout << "write room_id: [" << static_cast<int>(room_id_) << "]\n";
		ptr += sizeof(room_id_);
		std::memcpy(ptr, &sender_id_, sizeof(sender_id_));
		std::cout << "write sender_id: [" << static_cast<int>(sender_id_) << "]\n";
		ptr += sizeof(sender_id_);
		//std::cout << "try write body_length: [" << body_length_ << "]\n";
		std::memcpy(ptr, &body_length_, sizeof(body_length_));
		//std::cout << "wrote body length\n";
	}
	bool decode_header() {
		if (header_length > sizeof(data_)) { return false; }
		token_len = data_[0];
		token.assign(reinterpret_cast<char*>(data_ + 1), token_length);
		room_id = (data_)[token_length + 1];
		sender_id = data_[token_length + 2];
		body_length_ = static_cast<uint16_t>((data_)[token_length + 3]) |
			static_cast<uint16_t>((data_)[token_length + 4]) << 8;
		if (body_length_ > max_body_length) { 
			body_length_ = 0;
			return false; }
		return true;
	}
private:
	uint8_t data_[header_length + max_body_length];
};
//old version
/*class voice_chat_message {
public:
	static constexpr std::size_t max_encoded_length = 4000;
	static constexpr std::size_t token_length = 16;
	static constexpr std::size_t header_length = 21;
	uint8_t token_len;//1
	std::string token;//16
	uint8_t room_id;//1
	uint8_t sender_id;//1
	uint16_t len;//2
	void set_header(std::string token_, int encoded_bytes_, uint8_t room_id_, uint8_t sender_id_) {
		token_len = token_length;
		token = token_;
		encoded_bytes = encoded_bytes_;
		len = static_cast<uint16_t>(encoded_bytes);
		std::memcpy(data, &token_len, sizeof(token_len));
		std::memcpy(data + sizeof(token_len), token.c_str(), token_length);
		std::memcpy(data + sizeof(token_len) + token_length, &room_id_, sizeof(room_id_));
		std::memcpy(data + sizeof(token_len) + token_length + sizeof(room_id_), &sender_id_, sizeof(sender_id_));
		std::memcpy(data + sizeof(token_len) + token_length + sizeof(room_id_) + sizeof(sender_id), &len, sizeof(len));
	}
	void decode_header(uint8_t* array) {
		token_len = (array)[0];
		token.assign(reinterpret_cast<char*>(array + 1), token_length);
		room_id = (array)[token_length + 1];
		sender_id = array[token_length + 2];
		len = static_cast<uint16_t>((array)[token_length + 3]) |
			static_cast<uint16_t>((array)[token_length + 4]) << 8;
	}
	uint8_t* get_data() {
		return data;
	}
private:
	int encoded_bytes;
	uint8_t data[header_length];
};*/
#endif