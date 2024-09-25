#pragma once
#pragma comment(lib, "ws2_32")
#include <winsock2.h>
#include <WS2tcpip.h>

#include <thread>
#include <vector>

#define MAX_SOCKBUF 1024	// ��Ŷ ũ��
#define MAX_WORKERTHREAD 4 // ������ Ǯ�� ���� ������ ��

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

// Ŭ���̾�Ʈ ������ ������� ����ü
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

	// ������ �ʱ�ȭ�ϴ� �Լ�
	bool InitSocket()
	{
		WSADATA wsaData;

		int ret = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (0 != ret)
		{
			printf("[Error] Failed to WSAStartup() : %d\n", WSAGetLastError());
			return false;
		}

		// TCP, Overlapped I/O ������ ����
		_listenSocket = ::WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, WSA_FLAG_OVERLAPPED);

		if (INVALID_SOCKET == _listenSocket)
		{
			printf("[Error] Failed to socket() : %d\n", WSAGetLastError());
			return false;
		}

		printf("���� �ʱ�ȭ ����\n");
		return true;
	}

	//-------������ �Լ�---------//
	// ������ �ּ������� ���ϰ� ���� ��Ű�� ���� ��û�� �ޱ� ����
	// ������ ����ϴ� �Լ�
	bool BindAndListen(int bindPort)
	{
		SOCKADDR_IN stServerAddr;
		stServerAddr.sin_family = AF_INET;
		stServerAddr.sin_port = htons(bindPort); // ���� ��Ʈ�� ����
		// � �ּҿ��� ������ �����̶� �޾Ƶ��̰ڴ�.
		// ���� ������� �̷��� �����Ѵ�. ���� �� �����ǿ����� ������ �ް� �ʹٸ�
		// �� �ּҸ� inet_adr�Լ��� �̿��� ������ �ȴ�.
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

		printf("���� ��� ����..\n");
		return true;
	}

	// ���� ��û�� �����ϰ� �޼����� �޾Ƽ� ó���ϴ� �Լ�
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

		printf("���� ����\n");
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

	// IOCP �Ϸ� ť�� ����� ������ ����
	bool CreateWorkerThread()
	{
		unsigned int threadId = 0;
		for (int i = 0; i < MAX_WORKERTHREAD; ++i)
		{
			_ioWorkerThreads.emplace_back([this]() { WorkerThread(); });
		}

		printf("WorkerThread ����..\n");
		return true;
	}

	// Overlapped I/O�۾��� ���� �Ϸ� �뺸�� �޾�
	// �׿� �ش��ϴ� ó���� �ϴ� �Լ�
	void WorkerThread()
	{
		stClientInfo* clientInfo = nullptr;
		BOOL isSuccess = TRUE;
		DWORD numOfBytes = 0;
		LPOVERLAPPED overlapped = nullptr;

		while (_isWorkerRun)
		{
			/*
			* �� �Լ��� ���� ��������� WaitingThread Queue�� ��� ���·� ����.
			* �Ϸ�� Overlapped I/O �۾��� �߻��ϸ� IOCP Queue���� �Ϸ�� �۾��� ������ ó���Ѵ�.
			* �׸��� PostQueuedCompletionSatus()�Լ������� ����� �޼����� �����ϸ�
			* �����带 �����Ѵ�.
			*/
			isSuccess = ::GetQueuedCompletionStatus(_iocpHandle
				, &numOfBytes
				, (PULONG_PTR)&clientInfo
				, &overlapped
				, INFINITE);

			// ����� ������ ���� �޼��� ó��..
			if (TRUE == isSuccess && 0 == numOfBytes && NULL == overlapped)
			{
				_isWorkerRun = false;
				continue;
			}

			if (NULL == overlapped)
			{
				continue;
			}

			// client�� ������ ������ ��..
			if (FALSE == isSuccess || (0 == numOfBytes && TRUE == isSuccess))
			{
				printf("socket(%d) ���� ����\n", (int)clientInfo->SocketClient);
				CloseSocket(clientInfo);
				continue;
			}

			stOverlappedEx* overlappedEx = reinterpret_cast<stOverlappedEx*>(overlapped);

			// Overlapeed I/O Recv�۾� ��� �� ó��
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
				printf("socket(%d)���� ���ܻ�Ȳ\n", (int)overlappedEx->SocketClient);
			}
		}
	}

	// ������ ������ ���� ��Ų��.
	void CloseSocket(stClientInfo* clientInfo, bool isForce = false)
	{
		struct linger stLinger = { 0, 0 };

		// Linger �ɼ��̶�, ������ close �� �� ���۵��� ���� �����͸� ��� ó�������� ���� ����
		// �⺻������ TCP�� ������ ���� ���Ŀ��� �����ð� ���� �����͸� �������ش�.
		
		// l_onoff == 0 : Linger �ɼ� ��Ȱ��ȭ. ��� �����͸� �����ϴ� �Ϲ����� ������ ���� ��������
		// l_onoff > 0 and l_linger == 0 : close�� ��� �����ؼ� �����ִ� �����͸� ������ �ɼ� TCP�� ��� ���濡�� RST ��Ŷ�� ���޵ȴ�.
		// l_onoff > 0 and l_linger > 0 : ������ �ð����� ����ϰ� ���ۿ� �����ִ� �����͸� ��� ������.
		// ������ �ð� �̳��� �����͸� ��� ������ ���� ���� �׷��� ���ϸ� ������ �Բ� ������ �ȴ�.
		
		// isForce�� true�̸� SO_LINGER, timeout = 0���� �����Ͽ� ���� ���� ��Ų��. ���� : ������ �ս��� ���� �� ����.
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
			printf("[Error] WSASend() �Լ� ���� : %d\n", WSAGetLastError());
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

		printf("AccepterThread ����...\n");
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
			printf("Ŭ���̾�Ʈ ���� : IP(%s) SOCKET(%d)\n", clientIP, (int)clientInfo->SocketClient);

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

	// Ŭ���̾�Ʈ ���� ���� ����ü
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