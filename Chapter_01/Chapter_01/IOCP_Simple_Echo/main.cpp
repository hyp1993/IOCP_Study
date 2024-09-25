#include "IOCompletionPort.h"

const UINT16 SERVER_PORT = 30001;
const UINT16 MAX_CLIENT = 100;

int main()
{
	IOCompletionPort ioCompletionPort;

	// 소켓 초기화
	ioCompletionPort.InitSocket();

	// 소켓과 서버 주소를 연결하고 등록 시킨다.
	ioCompletionPort.BindAndListen(SERVER_PORT);

	ioCompletionPort.StartServer(MAX_CLIENT);

	printf("아무 키나 누를 때까지 대기합니다\n");
	getchar();

	ioCompletionPort.DestoryThread();
	return 0;
}