#include "IOCompletionPort.h"

const UINT16 SERVER_PORT = 30001;
const UINT16 MAX_CLIENT = 100;

int main()
{
	IOCompletionPort ioCompletionPort;

	// ���� �ʱ�ȭ
	ioCompletionPort.InitSocket();

	// ���ϰ� ���� �ּҸ� �����ϰ� ��� ��Ų��.
	ioCompletionPort.BindAndListen(SERVER_PORT);

	ioCompletionPort.StartServer(MAX_CLIENT);

	printf("�ƹ� Ű�� ���� ������ ����մϴ�\n");
	getchar();

	ioCompletionPort.DestoryThread();
	return 0;
}