#ifndef CHAT_PARTICIPANT_H
#define CHAT_PARTICIPANT_H
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include "Chat_Message.h"
#include "Voice_Chat_Message.h"
#include "boost/asio.hpp"
#include "Typedefs.h"


enum class voice_chat_state : uint8_t {
	none = 0,
	initiating = 1,
	recieving_request = 2,
	in_session = 3,
	self_test_session = 4
};
//
void hex_dump(const char* data, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		std::cout << std::hex << std::setw(2) << std::setfill('0')
			<< (static_cast<unsigned>(static_cast<uint8_t>(data[i]))) << " ";
		if ((i + 1) % 16 == 0) std::cout << "\n";
	}
	std::cout << std::dec << "\n"; // reset to decimal
}

static constexpr std::size_t participant_max_size = 32;
static constexpr std::size_t token_size = 16;
class participant_client_data;
class chat_participant {
public:
	std::string session_token; //16 bytes. don't serialize
	std::uint8_t id; //1 byte
	voice_chat_state vc_state; //1 byte
	std::string name; //16 bytes max
	//std::vector<uint8_t> partners; //16 bytes max

	virtual ~chat_participant() {}//virtual prevents only base destructor from running when inherited from
	//virtual void deliver(const chat_message& msg) = 0;
	virtual void deliver(const chat_message& msg) {};
	virtual void write_vc_msg_to_rb(std::shared_ptr<voice_chat_message> recv_vc_msg_) {};
	virtual void set_vc_room_id(uint8_t id) {};
	virtual uint8_t get_vc_room_id() { return 0; }
	virtual std::shared_ptr<boost::asio::ip::udp::endpoint> get_client_udp_endpoint() { return nullptr; };
	virtual void read_vc_rb() {};
	virtual void commit_read_rb(size_t size) {};
	virtual void start_read_vc_rb() {};
	virtual void stop_read_vc_rb() {};
	virtual bool get_feedback_option() { return false; };
	virtual void set_feedback_option(bool val) { }
	virtual std::unordered_set<uint8_t>* get_vc_partner_ids() {
		return nullptr;
	};
	virtual std::unordered_set<uint8_t>* get_vc_requestor_ids() {
		return nullptr;
	};
	virtual uint8_t get_vc_enabled() {
		return 0;
	}
	virtual void set_vc_enabled(uint8_t val) {
	}
	std::size_t serialized_size() {
		size_t size = 0;
		size += sizeof(id);
		size += sizeof(static_cast<uint8_t>(vc_state));
		size += sizeof(uint8_t);//name length
		size += name.size();
		return size;
	}
	std::size_t serialize(char* out) {
		char* ptr = out;

		std::memcpy(ptr, &id, sizeof(id));
		ptr += sizeof(id);

		uint8_t state = static_cast<uint8_t>(vc_state);
		std::memcpy(ptr, &state, sizeof(state));
		ptr += sizeof(state);

		uint8_t name_len = static_cast<uint8_t>(name.size());
		std::memcpy(ptr, &name_len, sizeof(name_len));
		ptr += sizeof(name_len);

		std::memcpy(ptr, name.data(), name_len);
		ptr += name_len;
		return ptr - out;
	}
	std::size_t deserialize(const char* in) {
		//std::cout << "deserialize chat_participant\n";
		const char* ptr = in;

		std::memcpy(&id, ptr, sizeof(id));
		//std::cout << "id: " << static_cast<int>(id) << "\n";
		ptr += sizeof(id);

		uint8_t state;
		std::memcpy(&state, ptr, sizeof(state));
		vc_state = static_cast<voice_chat_state>(state);
		//std::cout << "vc_state: " << static_cast<int>(vc_state) << "\n";
		ptr += sizeof(state);

		uint8_t name_len;
		std::memcpy(&name_len, ptr, sizeof(name_len));
		//std::cout << "name_len = " << static_cast<int>(name_len) << "\n";
		ptr += sizeof(name_len);

		name.assign(ptr, name_len);
		//std::cout << "name = " << name << "\n";
		ptr += name_len;

		return ptr - in;

		/*uint8_t partner_count;
		std::memcpy(&partner_count, ptr, sizeof(partner_count));
		ptr += sizeof(partner_count);

		for (uint8_t i = 0; i < partner_count; ++i) {
			uint8_t partner_id;
			std::memcpy(&partner_id, ptr, sizeof(partner_id));
			ptr += sizeof(partner_id);
			partners.push_back(partner_id);
		}*/
	}

};
enum participant_state {
	requesting_vc = 1,
	sending_vc_request = 2,
	vc_request_rejected = 3,
	in_vc = 4,
	neutral = 5
};

class participant_client_data {
public:
	participant_client_data(chat_participant p) : p{ p }, ps{ participant_state::neutral }, enable_vc{ false, false } {}
	participant_client_data(participant_client_data& obj) {
		p = obj.p;
		ps = obj.ps;
		enable_vc = obj.enable_vc;
		output_volume = 1.0f;
	}
	chat_participant p;
	participant_state ps;
	std::pair<bool, bool> enable_vc;//my value for them //their value for me //if both are 1 then vc is enabled. this is 
	//double checked on server.
	float output_volume;
};
struct participant_container {
	participant_container() : name{ "empty" }, vc_state{ 0 }
	{
	}
	uint32_t participant_id;
	voice_chat_state vc_state;
	const char* name;
	static constexpr std::size_t max_size = 128;
	void serialize(char* out) {
		snprintf(out, max_size, "%u%c%s%c%u%c",
			participant_id, '\0',
			name, '\0',
			static_cast<uint8_t>(vc_state), '\0');
	}
	void deserialize(const char* in) {
		const char* ptr = in;
		participant_id = std::strtoul(ptr, nullptr, 10);
		ptr += std::strlen(ptr) + 1;
		vc_state = static_cast<voice_chat_state>(std::strtoul(ptr, nullptr, 10));
		ptr += std::strlen(ptr) + 1;
		name = ptr;
	}
};
#endif