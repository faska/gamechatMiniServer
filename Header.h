#pragma once

#define LOG Log()
auto& Log()
{
	std::time_t time = std::time(nullptr);
	return std::cout << "\n\n" << std::asctime(std::localtime(&time));
}

template <class T, void(&handler)(const asio::error_code&, T&), size_t timeToWait>
void setTimer(T& obj)
{
	obj.m_timeoutTimer.expires_from_now(std::chrono::milliseconds(timeToWait));
	obj.m_timeoutTimer.async_wait(std::bind(handler, std::placeholders::_1, std::ref(obj)));
}

bool findString(const std::string & str1, const std::string & str2)
{
	auto it = std::search(
		str1.begin(), str1.end(),
		str2.begin(), str2.end(),
		[](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); }
	);
	return (it != str1.end());
}

template<size_t maxLength>
bool validateText(const std::string text)
{
	if ((text.size() > maxLength) || text.empty())
		return false;
	for (const auto& symbol : text)
	{
		if (!std::isprint(symbol, locale))
			return false;
	}
	return true;
}


class MessageOut
{

private:
	//Calculate Needed Space

	void CalculateSpaceNeededForBody(size_t&)
	{

	}

	template<class... T2>
	void CalculateSpaceNeededForBody(size_t& counter, const std::string& arg, const T2&... args)
	{
		counter += sizeof(uint32_t);
		counter += arg.size();
		CalculateSpaceNeededForBody(counter, args...);
	}
	template<class T1, class... T2>
	std::enable_if_t<std::is_same_v<T1, size_t>>
	CalculateSpaceNeededForBody(size_t& counter, const T1& arg, const T2&... args)
	{
		counter += sizeof(uint32_t);
		CalculateSpaceNeededForBody(counter, args...);
	}

	//Serialize
	void Serialize() {}

	template<class... T2>
	void Serialize(const std::string& arg, const T2&... args)
	{
		Serialize(arg.size());
		m_raw->append(arg);
		Serialize(args...);
	}
	template<class T1, class... T2>
	std::enable_if_t<std::is_same_v<T1, uint32_t>>
	Serialize(const T1& arg, const T2&... args)
	{
		m_raw->append(reinterpret_cast<const char*>(&arg), sizeof(uint32_t));
		Serialize(args...);
	}
public:
	const std::shared_ptr<std::string> m_raw = std::make_shared<std::string>();
public:
	template<class... T1>
 	explicit MessageOut(const HEADER& m_header,const T1&... args)
	{
		size_t bytesNeededForBody=0;
		CalculateSpaceNeededForBody(bytesNeededForBody,args...);
		m_raw->reserve(bytesNeededForBody+ sizeof(HEADER) + sizeof(uint32_t));
		m_raw->append(reinterpret_cast<const char*>(&m_header), sizeof(HEADER));
		Serialize(static_cast<uint32_t>(bytesNeededForBody),args...);
	}
	virtual ~MessageOut() = 0;
};

MessageOut::~MessageOut() = default;


class MessageOutVerified :public MessageOut
{
public:
	MessageOutVerified():
		MessageOut(HEADER::VERIFIED)
	{}
};

class MessageOutChatOK : public MessageOut
{
public:
	MessageOutChatOK(const std::string& inLoggedAs) :
		MessageOut(HEADER::CHAT_OK, inLoggedAs)
	{}
};

class MessageOutChatFail : public MessageOut
{
public:
	MessageOutChatFail() :
		MessageOut(HEADER::CHAT_FAIL)
	{}
};

class MessageOutChatNewParticipater :public MessageOut
{
public:
	MessageOutChatNewParticipater(const std::string& inNewbieName) :
		MessageOut(HEADER::CHAT_NEW_PARTICIPATER, inNewbieName)
	{}
};

class MessageOutChatLeftParticipater :public MessageOut
{
public:
	MessageOutChatLeftParticipater(const std::string& inLeavedName) :
		MessageOut(HEADER::CHAT_LEFT_PARTICIPATER, inLeavedName)
	{}
};

class MessageOutChatMsg :public MessageOut
{
public:
	MessageOutChatMsg(
		const std::string& nickName,
		const std::string& msgText,
		const std::string& iconName,
		const std::string& levelName,
		const std::string& factionName) :
		MessageOut(HEADER::CHAT_MSG, nickName,msgText,iconName,levelName,factionName)
	{}
};

class MessageOutHistoryEnd :public MessageOut
{
public:
		MessageOutHistoryEnd():MessageOut(HEADER::CHAT_HISTORY)
		{}
};

class MessageOutPing :public MessageOut
{
public:
	MessageOutPing() :MessageOut(HEADER::PING)
	{}
};

struct ChatParticipater
{
	ChatParticipater(const ADMIN_RIGHTS& inRights, const std::string& inName)
		:m_adminRights(inRights), m_name(inName) {}
	//std::string color;
	const ADMIN_RIGHTS                      m_adminRights;
	const std::string                       m_name;
};

class BaseClient
{
	bool   m_deadFlag = false;
public:
	tcp::socket m_socket;
	std::list<std::shared_ptr<std::string>>   m_dataToBeSent;
	const size_t                    m_id;
	std::array<uint8_t, 1024> m_buffer = { 0 };
	//BaseClient() = delete;
	explicit BaseClient(tcp::socket&& in_socket, const size_t& in_id)
		: m_socket(std::move(in_socket)), m_id(in_id) {}
	virtual ~BaseClient() = 0;
	asio::steady_timer m_timeoutTimer = asio::steady_timer(ioservice);

	void Kill()
	{
		this->m_timeoutTimer.cancel();

		m_deadFlag = true;
	}
	bool isKilled() const
	{
		return m_deadFlag;
	}

};
BaseClient::~BaseClient() = default;

class HeadReader
{
public:
	const HEADER&  m_header;
	const uint32_t& m_bodySize;
	HeadReader(const BaseClient& inClient)
		:m_header(*reinterpret_cast<const HEADER*>(inClient.m_buffer.data())),
		m_bodySize(*reinterpret_cast<const uint32_t*>(inClient.m_buffer.data() + sizeof(HEADER)))
	{

	}
};

class UnverifiedClient:public BaseClient
{
	std::string m_random64bytes;
public:
	using BaseClient::BaseClient;
	void GenerateRandom64()
	{
		m_random64bytes.reserve(64);
		std::random_device rd;
		for (size_t i = 0; i < 64; i++)
		{
			const char byte = rd() % 255;
			m_random64bytes.append(&byte, 1);
		}
	}

	const std::string& GetRandom64() const
	{
		return m_random64bytes;
	}

};

class VerifiedClient :public BaseClient
{
	
	bool   m_isPingWasSentToThisClient = false;
	
public:
	explicit VerifiedClient(UnverifiedClient&& old_client)
		:BaseClient(std::move(old_client.m_socket), old_client.m_id)
	{}
	const ChatParticipater*  m_chatParticipater=nullptr;

	bool isPingWasSentToThisClient() const
	{
		return m_isPingWasSentToThisClient;
	}
	void setPongIsExpectedToBeRecievedFromThisClient(const bool value)
	{
		m_isPingWasSentToThisClient = value;
	}
};

//dummy
constexpr bool Deserialize(const uint8_t* const, const uint8_t* const) { return true; }

template<class T1, class... T2>
typename std::enable_if_t<std::is_same<T1, uint32_t>::value,bool>
Deserialize(const uint8_t* const pToBegin, const uint8_t* const pToEnd, T1& arg, T2&... args)
{
	if (pToEnd - pToBegin < sizeof(T1))
	{
		return false;
	}
	arg = *reinterpret_cast<const T1*>(pToBegin);
	return Deserialize(pToBegin+sizeof(T1),pToEnd,args...);
}

template<class T1, class... T2>
typename std::enable_if_t<std::is_same<T1, std::string>::value, bool>
Deserialize(const uint8_t*pToBegin, const uint8_t*pToEnd, T1& arg, T2&... args)
{
	uint32_t lengthOfString;
	if (!Deserialize(pToBegin, pToEnd, lengthOfString))
		return false;
	if (pToBegin+sizeof(lengthOfString)+lengthOfString>pToEnd)
		return false;
	arg.assign(reinterpret_cast<const char*>(pToBegin + sizeof(lengthOfString)), lengthOfString);
	return Deserialize(pToBegin+sizeof(lengthOfString)+lengthOfString, pToEnd, args...);
}
