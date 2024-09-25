#pragma once
#pragma comment(lib, "ws2_32")
#include <winsock2.h>
#include <WS2tcpip.h>

#include <thread>
#include <vector>

#define MAX_SOCKBUF 1024	// 패킷 크기
#define MAX_WORKERTHREAD 4 // 스레드 풀에 넣을 스레드 수

enum class IOOperation
{
	RECV,
	SEND
};

struct stOverlappedEx
{
	WSAOVERLAPPED	WsaOverlapped;
	SOCKET			SocketClient;
	WSABUF			WsaBuf;
	char			Buffer[MAX_SOCKBUF];
	IOOperation		Operation;
};

// 클라이언트 정보를 담기위한 구조체
struct stClientInfo
{
	SOCKET SocketClient;
	stOverlappedEx	StRecvOverlappedEx;
	stOverlappedEx	StSendOverlappedEx;

	stClientInfo()
	{
		ZeroMemory(&StRecvOverlappedEx, sizeof(stOverlappedEx));
		ZeroMemory(&StSendOverlappedEx, sizeof(stOverlappedEx));
		SocketClient = INVALID_SOCKET;
	}
};

class IOCompletionPort
{
public:
	IOCompletionPort(void) = default;
	
	~IOCompletionPort()
	{
		WSACleanup();
	}

	// 소켓을 초기화하는 함수
	bool InitSocket()
	{
		WSADATA wsaData;

		int ret = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (0 != ret)
		{
			printf("[Error] Failed to WSAStartup() : %d\n", WSAGetLastError());
			return false;
		}

		// TCP, Overlapped I/O 소켓을 생성
		_listenSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, WSA_FLAG_OVERLAPPED);

		if (INVALID_SOCKET == _listenSocket)
		{
			printf("[Error] Failed to socket() : %d\n", WSAGetLastError());
			return false;
		}

		printf("소켓 초기화 성공\n");
		return true;
	}

	//-------서버용 함수---------//
	// 서버의 주소정보를 소켓과 연결 시키고 접속 요청을 받기 위해
	// 소켓을 등록하는 함수
	bool BindAndListen(int bindPort)
	{
		SOCKADDR_IN stServerAddr;
		stServerAddr.sin_family = AF_INET;
		stServerAddr.sin_port = htons(bindPort); // 서버 포트를 설정
		// 어떤 주소에서 들어오는 접속이라도 받아들이겠다.
		// 보통 서버라면 이렇게 설정한다. 만약 한 아이피에서만 접속을 받고 싶다면
		// 그 주소를 inet_adr함수를 이용해 넣으면 된다.
		stServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);

		int ret = ::bind(_listenSocket, (SOCKADDR*)&stServerAddr, sizeof(SOCKADDR_IN));
		if (SOCKET_ERROR == ret)
		{
			printf("[Error] Failed to bind() : %d\n", WSAGetLastError());
			return false;
		}

		ret = ::listen(_listenSocket, 5);
		if (SOCKET_ERROR == ret)
		{
			printf("[Error] Failed to listen() : %d\n", WSAGetLastError());
			return false;
		}

		printf("서버 등록 성공..\n");
		return true;
	}

	// 접속 요청을 수락하고 메세지를 받아서 처리하는 함수
	bool StartServer(const UINT32 maxClientCount)
	{
		CreateClient(maxClientCount);

		_iocpHandle = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, MAX_WORKERTHREAD);
		if (NULL == _iocpHandle)
		{
			printf("[Error] Failed to CreateIoCompletionPort() : %d\n", GetLastError());
			return false;
		}

		bool ret = CreateWorkerThread();
		if (false == ret)
		{
			return false;
		}

		ret = CreateAccepterThread();
		if (false == ret)
		{
			return false;
		}

		printf("서버 시작\n");
		return true;
	}

	void DestoryThread()
	{
		_isWorkerRun = false;
		CloseHandle(_iocpHandle);
		for (std::thread& thread : _ioWorkerThreads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		_isAccepterRun = false;
		closesocket(_listenSocket);
		if (_accepterThread.joinable())
		{
			_accepterThread.join();
		}
	}

private:
	void CreateClient(const UINT32 maxClientCount)
	{
		for (UINT32 i = 0; i < maxClientCount; ++i)
		{
			_clientInfos.emplace_back();
		}
	}

	// IOCP 완료 큐를 대기할 스레드 생성
	bool CreateWorkerThread()
	{
		unsigned int threadId = 0;
		for (int i = 0; i < MAX_WORKERTHREAD; ++i)
		{
			_ioWorkerThreads.emplace_back([this]() { WorkerThread(); });
		}

		printf("WorkerThread 시작..\n");
		return true;
	}

	// Overlapped I/O작업에 대한 완료 통보를 받아
	// 그에 해당하는 처리를 하는 함수
	void WorkerThread()
	{
		stClientInfo* clientInfo = nullptr;
		BOOL isSuccess = TRUE;
		DWORD numOfBytes = 0;
		LPOVERLAPPED overlapped = nullptr;

		while (_isWorkerRun)
		{
			/*
			* 이 함수로 인해 스레드들은 WaitingThread Queue에 대기 상태로 들어간다.
			* 완료된 Overlapped I/O 작업이 발생하면 IOCP Queue에서 완료된 작업을 가져와 처리한다.
			* 그리고 PostQueuedCompletionSatus()함수에의해 사용자 메세지가 도착하면
			* 스레드를 종료한다.
			*/
			isSuccess = ::GetQueuedCompletionStatus(_iocpHandle
				, &numOfBytes
				, (PULONG_PTR)&clientInfo
				, &overlapped
				, INFINITE);

			// 사용자 스레드 종료 메세지 처리..
			if (TRUE == isSuccess && 0 == numOfBytes && NULL == overlapped)
			{
				_isWorkerRun = false;
				continue;
			}

			if (NULL == overlapped)
			{
				continue;
			}

			// client가 접속을 끊었을 때..
			if (FALSE == isSuccess || (0 == numOfBytes && TRUE == isSuccess))
			{
				printf("socket(%d) 접속 끊김\n", (int)clientInfo->SocketClient);
				CloseSocket(clientInfo);
				continue;
			}

			stOverlappedEx* overlappedEx = reinterpret_cast<stOverlappedEx*>(overlapped);

			// Overlapeed I/O Recv작업 결과 뒤 처리
			if (IOOperation::RECV == overlappedEx->Operation)
			{
				overlappedEx->Buffer[numOfBytes] = NULL;
				printf("[Recv] bytes : %d , msg : %s\n", numOfBytes, overlappedEx->Buffer);

				SendMsg(clientInfo, overlappedEx->Buffer, numOfBytes);
				BindRecv(clientInfo);
			}
			else if (IOOperation::SEND == overlappedEx->Operation)
			{
				printf("[Send] bytes : %d , msg : %s\n", numOfBytes, overlappedEx->Buffer);
			}
			else
			{
				printf("socket(%d)에서 예외상황\n", (int)overlappedEx->SocketClient);
			}
		}
	}

	// 소켓의 연결을 종료 시킨다.
	void CloseSocket(stClientInfo* clientInfo, bool isForce = false)
	{
		struct linger stLinger = { 0, 0 };

		// Linger 옵션이란, 소켓을 close 할 때 전송되지 않은 데이터를 어떻게 처리할지에 대한 설정
		// 기본적으로 TCP는 연결이 끊긴 이후에도 일정시간 동안 데이터를 보장해준다.
		
		// l_onoff == 0 : Linger 옵션 비활성화. 모든 데이터를 전송하는 일반적인 소켓의 정상 종료형태
		// l_onoff > 0 and l_linger == 0 : close가 즉시 리턴해서 남아있는 데이터를 버리는 옵션 TCP의 경우 상대방에게 RST 패킷이 전달된다.
		// l_onoff > 0 and l_linger > 0 : 지정한 시간동안 대기하고 버퍼에 남아있는 데이터를 모두 보낸다.
		// 지정한 시간 이내에 데이터를 모두 보내면 정상 종료 그렇지 못하면 에러와 함께 리턴이 된다.
		
		// isForce가 true이면 SO_LINGER, timeout = 0으로 설정하여 강제 종료 시킨다. 주의 : 데이터 손실이 있을 수 있음.
		if (true == isForce)
		{
			stLinger.l_onoff = 1;
		}

		shutdown(clientInfo->SocketClient, SD_BOTH);

		setsockopt(clientInfo->SocketClient, SOL_SOCKET, SO_LINGER, (char*)&stLinger, sizeof(stLinger));

		closesocket(clientInfo->SocketClient);

		clientInfo->SocketClient = INVALID_SOCKET;
	}

	bool SendMsg(stClientInfo* clientInfo, char* msg, int len)
	{
		DWORD recvNumOfBytes = 0;

		CopyMemory(clientInfo->StSendOverlappedEx.Buffer, msg, len);

		clientInfo->StSendOverlappedEx.WsaBuf.len = len;
		clientInfo->StSendOverlappedEx.WsaBuf.buf = clientInfo->StSendOverlappedEx.Buffer;
		clientInfo->StSendOverlappedEx.Operation = IOOperation::SEND;

		int ret = WSASend(clientInfo->SocketClient
			, &clientInfo->StSendOverlappedEx.WsaBuf
			, 1
			, &recvNumOfBytes
			, 0
			, (LPWSAOVERLAPPED) & (clientInfo->StSendOverlappedEx)
			, NULL);

		if (ret == SOCKET_ERROR && (WSAGetLastError() != WSA_IO_PENDING))
		{
			printf("[Error] WSASend() 함수 실패 : %d\n", WSAGetLastError());
			return false;
		}
		return true;
	}

	bool BindRecv(stClientInfo* clientInfo)
	{
		DWORD recvNumOfBytes = 0;
		DWORD flag = 0;

		clientInfo->StRecvOverlappedEx.WsaBuf.buf = clientInfo->StRecvOverlappedEx.Buffer;
		clientInfo->StRecvOverlappedEx.WsaBuf.len = MAX_SOCKBUF;
		clientInfo->StRecvOverlappedEx.Operation = IOOperation::RECV;

		int ret = WSARecv(clientInfo->SocketClient
			, &clientInfo->StRecvOverlappedEx.WsaBuf
			, 1
			, &recvNumOfBytes
			, &flag
			, (LPWSAOVERLAPPED) & (clientInfo->StRecvOverlappedEx.WsaOverlapped)
			, NULL);

		if (ret == SOCKET_ERROR && (WSAGetLastError() != WSA_IO_PENDING))
		{
			printf("[Error] Failed to WSARecv() : %d\n", WSAGetLastError());
			return false;
		}
		return true;
	}

	bool CreateAccepterThread()
	{
		_accepterThread = std::thread([this]() { AccepterThread(); });

		printf("AccepterThread 시작...\n");
		return true;
	}

	void AccepterThread()
	{
		SOCKADDR_IN clientAddr;
		int addrLen = sizeof(SOCKADDR_IN);

		while (_isAccepterRun)
		{
			stClientInfo* clientInfo = GetEmptyClientInfo();
			if (nullptr == clientInfo)
			{
				printf("[Error] Full client\n");
				return;
			}

			clientInfo->SocketClient = ::accept(_listenSocket, (SOCKADDR*)&clientAddr, &addrLen);
			if (INVALID_SOCKET == clientInfo->SocketClient)
			{
				continue;
			}

			int ret = BindIOCompletionPort(clientInfo);
			if (false == ret)
			{
				continue;
			}

			ret = BindRecv(clientInfo);
			if (false == ret)
			{
				continue;
			}

			char clientIP[32] = { 0, };
			::inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIP, 32 - 1);
			printf("클라이언트 접속 : IP(%s) SOCKET(%d)\n", clientIP, (int)clientInfo->SocketClient);

			++_clientCount;
		}
	}

	stClientInfo* GetEmptyClientInfo()
	{
		for (auto& client : _clientInfos)
		{
			if (INVALID_SOCKET == client.SocketClient)
			{
				return &client;
			}
		}
		return nullptr;
	}

	bool BindIOCompletionPort(stClientInfo* clientInfo)
	{
		HANDLE iocp = ::CreateIoCompletionPort((HANDLE)clientInfo->SocketClient, _iocpHandle, (ULONG_PTR)clientInfo, 0);
		if (NULL == iocp || iocp != _iocpHandle)
		{
			printf("[Error] Failed to CreateIoCompletionPort() : %d\n", GetLastError());
			return false;
		}
		return true;
	}

private:

	// 클라이언트 정보 저장 구조체
	std::vector<stClientInfo> _clientInfos;

	SOCKET _listenSocket = INVALID_SOCKET;

	// IO Worker Thread
	std::vector<std::thread> _ioWorkerThreads;

	HANDLE _iocpHandle = INVALID_HANDLE_VALUE;

	bool _isWorkerRun = true;

	std::thread _accepterThread;

	bool _isAccepterRun = true;

	int _clientCount = 0;
};