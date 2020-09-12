
#include "stdafx.h"
#include "plusaes.hpp"


asio::io_service  ioservice;
namespace Constants
{
	constexpr int MIN_CACHED_MESSAGES = 20;
	constexpr int MAX_CACHED_MESSAGES = 40;
	constexpr int UNVERIFIED_CLIENT_TIMEOUT = 3'000;
	constexpr int VERIFIED_CLIENT_TIMEOUT   = 10'000;
	constexpr int DELAYED_WORKER_DELAY = 10'000;
	constexpr int NICKNAME_LENGTH_MAX = 20;
	constexpr int TOKEN_LENGTH_MAX = 20;
	constexpr std::array<uint8_t, 16> KEY{ 0x29,0x9C,0x5B,0x59,0x15,0x3B,0xA8,0x07,0x6E,0x6B,0x38,0x4A,0x77,0x82,0x93,0xAB };
}

using namespace asio::ip;
using namespace std::chrono;
using namespace std::string_literals;

enum class HEADER
{
	//ABOUT CONNECTION
	HANDSHAKE=0x00u,
	VERIFIED,
	PING,
	PONG,
	//ABOUT CHAT
	CHAT_MSG,
	CHAT_HISTORY,
	CHAT_LOGIN,
	CHAT_FAIL,
	CHAT_OK,
	CHAT_NEW_PARTICIPATER,
	CHAT_LEFT_PARTICIPATER
};

enum class ADMIN_RIGHTS
{
	NONE,
	ADMIN
};

#include "Header.h"

tcp::endpoint     tcp_endpoint{ tcp::v4(), 12000 };
tcp::acceptor     tcp_acceptor{ ioservice, tcp_endpoint };
tcp::socket       tcp_socket{ ioservice };
size_t            instancesCounter;
std::locale       locale("ru-RU");

std::list<VerifiedClient>   auth_clients;
std::list<UnverifiedClient> unauth_clients;
asio::steady_timer delayedWorkerTimer = asio::steady_timer(ioservice);
//History of the chat is here
std::vector<std::unique_ptr<MessageOut>>          cachedChat;
std::unordered_map<std::string, ChatParticipater> usersMap;

void writeHandler(const asio::error_code& ec, std::size_t, VerifiedClient& client)
{
	if (ec)
	{
		LOG << "Client #" << client.m_id << " is KICKED. Socket error while sending: " << ec.message();
		client.Kill();
		return;
	}
	client.m_dataToBeSent.pop_front();
	if (client.m_dataToBeSent.empty())
		return;
	async_write(client.m_socket,
		asio::buffer(*client.m_dataToBeSent.front()),
		std::bind(writeHandler, std::placeholders::_1, std::placeholders::_2, std::ref(client)));
}


void writeToVerifiedClient(VerifiedClient& client, const MessageOut& msgToSend)
{
	if (client.isKilled())
		return;
	client.m_dataToBeSent.emplace_back(msgToSend.m_raw);
	if (client.m_dataToBeSent.size()!=1) //something were before us, so it means async_write is in process
	{
		return;
	}

	async_write(client.m_socket,
		asio::buffer(*client.m_dataToBeSent.front()),
		std::bind(writeHandler, std::placeholders::_1, std::placeholders::_2, std::ref(client)));
}


void writeToAllVerifiedClients(const MessageOut& msgToSend)
{
	for (auto& authClient : auth_clients)
	{
		writeToVerifiedClient(authClient, msgToSend);
	}
}

void timeout_handler_unverified(const asio::error_code& ec, UnverifiedClient& client)
{
	if (ec) //client answered
		return;
	LOG<<"Client #" << client.m_id << ". TIMEOUT was reached";
	client.m_socket.cancel();
}
void timeout_handler_verified(const asio::error_code& ec, VerifiedClient& client)
{
	if (ec) //client answered
		return;
	if (client.isPingWasSentToThisClient())
	{
		LOG<<"Client #" << client.m_id << ". TIMEOUT was reached";
		client.m_socket.cancel();
	}
	else
	{
		writeToVerifiedClient(client, MessageOutPing());
		client.setPongIsExpectedToBeRecievedFromThisClient(true);
		setTimer<VerifiedClient, timeout_handler_verified, Constants::VERIFIED_CLIENT_TIMEOUT>(client);
	}
	return;

}



void callback_CHAT_MSG_body(VerifiedClient& client, const std::size_t numOfBytesRecieved)
{
	std::string msgText;
	std::string levelName;
	std::string factionName;
	if (!Deserialize(client.m_buffer.data(), client.m_buffer.data() + numOfBytesRecieved,
		msgText,
		levelName,
		factionName))
	{
		LOG<<"Client #" << client.m_id << " is KICKED. " << "Failed to deserialize message's body for CHAT_MSG";
		client.Kill();
		return;
	}
	const char* placeWhereErrorHappend=nullptr;
	if (!validateText<255>(msgText))
	{
		placeWhereErrorHappend = "message text!";
	}
	else if(!validateText<20>(levelName))
	{
		placeWhereErrorHappend = "level name!";
	}
	else if (!validateText<20>(factionName))
	{
		placeWhereErrorHappend = "faction name!";
	}

	if (placeWhereErrorHappend)
	{
		LOG<<"Client #" << client.m_id << " is KICKED. " << "Unvalid "<< placeWhereErrorHappend;
		client.Kill();
		return;
	}
	std::string temp = client.m_chatParticipater->m_name; //todo
	cachedChat.emplace_back(new  MessageOutChatMsg(client.m_chatParticipater->m_name,msgText,temp,levelName,factionName));
	writeToAllVerifiedClients(*cachedChat.back());
}

void callback_CHAT_LOGIN_body(VerifiedClient& client,const std::size_t numOfBytesRecieved)
{
	std::string requestedToken;
	if (!Deserialize(client.m_buffer.data(), client.m_buffer.data()+ numOfBytesRecieved,
		requestedToken)) //->1
	{
		LOG<<"Client #" << client.m_id << " is KICKED. " << "Failed to deserialize message's body for CHAT_LOGIN";
		client.Kill();
		return;
	}

	try
	{
		const ChatParticipater* requestedChatParticipater = &usersMap.at(requestedToken);
		
		for (const auto& authClient : auth_clients)
		{
			if (authClient.m_chatParticipater == requestedChatParticipater)
			{
				LOG << "Client #" << client.m_id << " failed to login. ChatParticipater '" << authClient.m_chatParticipater->m_name << "'is already issued!";
				writeToVerifiedClient(client, MessageOutChatFail());
				return;
			}
		}
		client.m_chatParticipater = requestedChatParticipater;
		LOG << "Client #" << client.m_id << " is logged in as " << client.m_chatParticipater->m_name;
		writeToVerifiedClient(client, MessageOutChatOK(client.m_chatParticipater->m_name));
		cachedChat.emplace_back(new MessageOutChatNewParticipater(client.m_chatParticipater->m_name));
		writeToAllVerifiedClients(*cachedChat.back());
	}
	catch(std::out_of_range)
	{
		LOG << "Client #" << client.m_id << " failed to login. Token '" << requestedToken << "' is unknown!";
		writeToVerifiedClient(client, MessageOutChatFail());
	}
}
/*forward decl->*/void asyncReadHeadFromAuthClient(VerifiedClient&);
template<void (&callback)(VerifiedClient& client, const std::size_t numOfBytesRecieved)>
void receivedBody_handler(const asio::error_code& ec, const std::size_t& _numOfBytesRecieved, VerifiedClient& client)
{
	if (ec)
	{
		LOG<<"Client #" << client.m_id << " is KICKED. " << "Failed while waiting for message's body, socket error: " << ec.message();
		client.Kill();
		return;
	}
	
	callback(client, _numOfBytesRecieved);
	asyncReadHeadFromAuthClient(client);
}
void receivedHead_handler(const asio::error_code& ec, const size_t&, VerifiedClient& client)
{
	if (ec)
	{
		LOG<<"Client #" << client.m_id << " is KICKED. " << "Fail while waiting for message header, socket error: " << ec.message();
		client.Kill();
		return;
	}
	const HeadReader hr(client);
	if (hr.m_bodySize > client.m_buffer.size())
	{
		LOG<<"Client #" << client.m_id << " is KICKED. Size requested for body message is larger than client's buffer able to contain!";
		client.Kill();
		return;
	}

	void (*bodyHandler) (const asio::error_code&, const std::size_t&, VerifiedClient&)=nullptr;

	switch(hr.m_header)
	{
	case HEADER::PONG:
		if (!client.isPingWasSentToThisClient())
		{
			LOG<<"Client #" << client.m_id << " is KICKED. PONG is recieved without PING being sent";
			client.Kill();
			return;
		}
		if (hr.m_bodySize)
		{
			LOG<<"Client #" << client.m_id << " is KICKED. PONG should be bodyless!";
			client.Kill();
			return;
		}
		client.setPongIsExpectedToBeRecievedFromThisClient(false);
		break;
	case HEADER::CHAT_MSG:
		if (!client.m_chatParticipater)
		{
			LOG<<"Client #" << client.m_id << " is KICKED. " << "Attempt to chat, but client is not chat participater!";
			client.Kill();
			return;
		}
		bodyHandler = receivedBody_handler<callback_CHAT_MSG_body>;
		break;
	case HEADER::CHAT_LOGIN:
		if (client.m_chatParticipater)
		{
			LOG<<"Client #" << client.m_id << " is KICKED. " << "Protocol mismatch. Already logged in!";
			client.Kill();
			return;
		}
		bodyHandler = receivedBody_handler<callback_CHAT_LOGIN_body>;
		break;
	default:
		LOG<<"Client #" << client.m_id << " is KICKED. Unknown header!";
		client.Kill();
		return;
	break;
	}

	if (!bodyHandler) //looks like recieved msg type is bodyless
	{
		//...so wait for next head
		asyncReadHeadFromAuthClient(client);
		return;
	}

	asio::async_read(
		client.m_socket,
		asio::buffer(client.m_buffer, hr.m_bodySize),
		std::bind(bodyHandler, std::placeholders::_1, std::placeholders::_2, std::ref(client))
	);
	setTimer<VerifiedClient, timeout_handler_verified, Constants::VERIFIED_CLIENT_TIMEOUT>(client);
}

void asyncReadHeadFromAuthClient(VerifiedClient& client)
{
	if (client.isKilled())
		return;
	asio::async_read(
		client.m_socket,
		asio::buffer(client.m_buffer, sizeof(HEADER) + sizeof(uint32_t)),
		std::bind(receivedHead_handler, std::placeholders::_1, std::placeholders::_2, std::ref(client))
	);
	setTimer<VerifiedClient, timeout_handler_verified, Constants::VERIFIED_CLIENT_TIMEOUT>(client);

}


void receivedEncryptedMsg_handler(const asio::error_code& ec, std::size_t, UnverifiedClient& client )
{
	if (ec)
	{
		LOG<<"Client #" << client.m_id << " is KICKED. Failed while waiting for crypted";
		client.Kill();
		return;
	}

	

	std::array<uint8_t, 64> decrypted;
	const auto result = plusaes::decrypt_cbc(
		static_cast<const uint8_t*>(client.m_buffer.data() + 16), // 16 bytes for IV
		64,
		Constants::KEY.data(),
		Constants::KEY.size(),
		reinterpret_cast<const uint8_t(*)[16]>(client.m_buffer.data()),
		decrypted.data(),
		decrypted.size(),
		nullptr);
	if (result != plusaes::Error::kErrorOk)
	{
		LOG<<"Client #" << client.m_id << " is KICKED. Failed to decrypt 64 bytes";
		client.Kill();
		return;
	}
	
	if (!std::memcmp(decrypted.data(), client.GetRandom64().data(), decrypted.size()))
	{
		auth_clients.emplace_back(std::move(client));
		client.Kill();
		auto& justVerifiedClient = auth_clients.back();
		LOG<<"Client #" << justVerifiedClient.m_id << " validation SUCCESS!";
		writeToVerifiedClient(justVerifiedClient, MessageOutVerified());
		for (const auto& cachedMsg : cachedChat)
		{
			writeToVerifiedClient(justVerifiedClient, *cachedMsg);
		}
		writeToVerifiedClient(justVerifiedClient, MessageOutHistoryEnd());
		asyncReadHeadFromAuthClient(justVerifiedClient);
	}
	else
	{
		LOG<<"Client #" << client.m_id << " is KICKED. Crypted ~= Decrypted";
		client.Kill();
	}
}

void Handshake_handler(const asio::error_code& ec, std::size_t, UnverifiedClient& client)
{
	if (ec)
	{
		LOG << "Client #" << client.m_id << " is KICKED. Failed while waiting for handshake";
		client.Kill();
		return;
	}

	

	const HeadReader hr(client);
	if(hr.m_header!=HEADER::HANDSHAKE || hr.m_bodySize!=0)
	{
		LOG << "Client #" << client.m_id << " is KICKED. Handshake protocol mismatch";
		client.Kill();
		return;
	}
	client.GenerateRandom64();
	auto handler = [&](const asio::error_code& ec, const std::size_t)
	{
		if (ec)
		{
			LOG<<"Client #" << client.m_id << " is KICKED. Failed to send 64 bytes";
			client.Kill();
		}
	};
	//Отправили 64 байта клиенту - пускай кодирует их
	asio::async_write(
		client.m_socket,
		asio::buffer(client.GetRandom64(), client.GetRandom64().size()),
		handler
		);
	//...и получаем то, что у него получилось
	asio::async_read(
		client.m_socket,
		asio::buffer(client.m_buffer, 64 + 16), //64 bytes of randomness + 16 bytes for IV
		std::bind(receivedEncryptedMsg_handler, std::placeholders::_1, std::placeholders::_2, std::ref(client))
		);
	setTimer<UnverifiedClient, timeout_handler_unverified, Constants::UNVERIFIED_CLIENT_TIMEOUT>(client);
}

void accept_handler(const asio::error_code& ec)
{
	if (!ec)
	{
		unauth_clients.emplace_back(std::move(tcp_socket), instancesCounter++);
		auto& justConnectedClient = unauth_clients.back();
		LOG<<"Client #" << justConnectedClient.m_id << ". Accepted connection from " << justConnectedClient.m_socket.remote_endpoint().address().to_string();
		asio::async_read(
			justConnectedClient.m_socket,
			asio::buffer(justConnectedClient.m_buffer, sizeof(HEADER) + sizeof(uint32_t)),
			std::bind(Handshake_handler, std::placeholders::_1, std::placeholders::_2, std::ref(justConnectedClient))
			);
		setTimer<UnverifiedClient, timeout_handler_unverified, Constants::UNVERIFIED_CLIENT_TIMEOUT>(justConnectedClient);
	}
	else
	{
		LOG << "ERROR! accept_handler failed for some strange reason o_O";
	}
	tcp_socket = tcp::socket(ioservice);
	tcp_acceptor.async_accept(tcp_socket, accept_handler);
}

bool readUsers()
{
	std::ifstream config("demosfen_users.txt");
	if (!config.is_open())
	{
		LOG<<"CAN'T FIND USERS FILE!";
		return false;
	}
	if (config.peek() == std::ifstream::traits_type::eof())
	{
		LOG << "USERS FILE IS EMPTY!";
		return false;
	}
	std::string nick;
	std::string token;
	std::string rights;
	//for log
	std::string table;
	constexpr char TOKEN[] = "[TOKEN]";
	constexpr char NICK[] = "[NICK]";
	constexpr char RIGHTS[] = "[RIGHTS]";
	auto insertRowInTable = [&](const std::string& st1, const std::string& st2, const std::string& st3, const std::string& st4)
	{
		constexpr char border[] = "|";
		size_t offset;
		table.append(st1).append(border);
		offset = (Constants::TOKEN_LENGTH_MAX - st2.size())/2;
		table.append(offset,' ').append(st2).append( offset,' ').append(border);
		offset = (Constants::NICKNAME_LENGTH_MAX - st3.size()) / 2;
		table.append(offset,' ').append(st3).append(offset,' ').append(border);
		offset = (10 - st4.size()) / 2;
		table.append(offset, ' ').append(st4).append(offset, ' ').append("\n");
	};
	insertRowInTable("#"s, TOKEN, NICK, RIGHTS);

	for (int counter = 1;;)
	{
		config >> token;
		config >> nick;
		config >> rights;
		if (!validateText<Constants::TOKEN_LENGTH_MAX>(token))
		{
			LOG << "Token for user #" << counter << " is unvalid! Should be <=" << Constants::TOKEN_LENGTH_MAX << "!";
			return false;
		}
		if (usersMap.count(token))
		{
			LOG << "Dublicate token for user #" << counter << "! Should be unique!";
			return false;
		}
		if (!validateText<Constants::NICKNAME_LENGTH_MAX>(nick))
		{
			LOG << "Nickname for user #" << counter << " is unvalid! Should be <="<< Constants::NICKNAME_LENGTH_MAX<<"!";
			return false;
		}
		if (rights == "NONE")
		{
			usersMap.emplace(std::piecewise_construct, std::forward_as_tuple(token), std::forward_as_tuple(ADMIN_RIGHTS::NONE, nick) );
		}
		else if (rights == "ADMIN")
		{
			usersMap.emplace(std::piecewise_construct, std::forward_as_tuple(token), std::forward_as_tuple(ADMIN_RIGHTS::NONE, nick));
		}
		else
		{
			LOG << "Rights for user #" << counter << " is unvalid! Should be NONE or ADMIN!";
			return false;
		}
		insertRowInTable(std::to_string(counter), token, nick, rights);
		if (config.eof()) 
		{
			break;
		}
		counter++;
	}
	LOG << "Users is loaded:\n" << table;
	return true;
}

void scheduleDelayedWorker()
{
	void delayedWorker(const asio::error_code&); //frwrd decl
	delayedWorkerTimer.expires_from_now(std::chrono::milliseconds(Constants::DELAYED_WORKER_DELAY));
	delayedWorkerTimer.async_wait(delayedWorker);
}

void delayedWorker(const asio::error_code& ec)
{
	if (ec)
	{
		LOG << "ERROR! delayedWorker failed...this should not happen!";
	}
	else
	{
		size_t numOfCleaned = 0;
		for (auto it = unauth_clients.begin(); it != unauth_clients.end();)
		{
			if (it->isKilled())
			{
				it = unauth_clients.erase(it);
				++numOfCleaned;
			}
			else
			{
				++it;
			}
		}
		for (auto it = auth_clients.begin(); it != auth_clients.end();)
		{
			if (it->isKilled())
			{
				if (it->m_chatParticipater)
				{
					cachedChat.emplace_back(new MessageOutChatLeftParticipater(it->m_chatParticipater->m_name));
					writeToAllVerifiedClients(*cachedChat.back());
				}
				it = auth_clients.erase(it);
				++numOfCleaned;
			}
			else
			{
				++it;
			}	
		}
		if(numOfCleaned)
			LOG << "DELAYED WORKER\nCleaned: "<< numOfCleaned <<"\nAuth clients: "<<auth_clients.size();
	}
	scheduleDelayedWorker();
}



int main()
{
	setlocale(LC_ALL, "Russian");
	std::ios::sync_with_stdio(false);

	if (!readUsers())
	{
		std::cin.get();
		return 0;
	}

	LOG << "START";
	tcp_acceptor.listen();
	tcp_acceptor.async_accept(tcp_socket, accept_handler);
	scheduleDelayedWorker();
	ioservice.run();
	std::cin.get();
	return 0;
}