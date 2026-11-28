#include "UELinkHeartbeat.h"

#include "Common/TcpListener.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogUELinkHeartbeat, Log, All);

namespace
{
	/** 游戏线程最近一次走 ticker 的时间和帧号：游戏线程写、监听线程读，所以用原子量 */
	std::atomic<double> HeartbeatLastTick{0.0};
	std::atomic<uint64> HeartbeatLastFrame{0};
	TUniquePtr<FTcpListener> HeartbeatListener;
	FTSTicker::FDelegateHandle HeartbeatTickHandle;

	bool HeartbeatTick(float)
	{
		HeartbeatLastTick.store(FPlatformTime::Seconds());
		HeartbeatLastFrame.store(GFrameCounter);
		return true;
	}

	/** 监听线程：只回一行，不碰 UObject、不排队到游戏线程（那正是卡住的地方） */
	bool HeartbeatAccept(FSocket* Socket, const FIPv4Endpoint&)
	{
		const double Last = HeartbeatLastTick.load();
		const int64 AgoMs = Last > 0.0 ? (int64)((FPlatformTime::Seconds() - Last) * 1000.0) : -1;
		const FString Line = FString::Printf(TEXT("{\"ok\":true,\"pid\":%u,\"frame\":%llu,\"lastTickAgoMs\":%lld}\n"), FPlatformProcess::GetCurrentProcessId(), (unsigned long long)HeartbeatLastFrame.load(), (long long)AgoMs);
		const FTCHARToUTF8 Utf8(*Line);
		int32 Sent = 0;
		Socket->SetNonBlocking(false);
		Socket->Send((const uint8*)Utf8.Get(), Utf8.Length(), Sent);
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		return true;
	}
}

int32 UELinkHeartbeat::Start()
{
	if (HeartbeatListener.IsValid())
	{
		FSocket* Socket = HeartbeatListener->GetSocket();
		return Socket != nullptr ? Socket->GetPortNo() : 0;
	}
	HeartbeatTick(0.0f);
	// 端口 0：让系统挑空闲端口；睡眠 50 毫秒是监听线程两次 accept 之间的间隔，面板问一次最多多等这么久
	HeartbeatListener = MakeUnique<FTcpListener>(FIPv4Endpoint(FIPv4Address(127, 0, 0, 1), 0), FTimespan::FromMilliseconds(50), false);
	HeartbeatListener->OnConnectionAccepted().BindStatic(&HeartbeatAccept);
	int32 Port = 0;
	for (int32 Attempt = 0; Attempt < 20 && Port == 0; ++Attempt)
	{
		if (FSocket* Socket = HeartbeatListener->GetSocket())
		{
			Port = Socket->GetPortNo();
		}
		if (Port == 0)
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}
	if (Port == 0)
	{
		UE_LOG(LogUELinkHeartbeat, Warning, TEXT("UELink：心跳端口没拿到，面板分不出编辑器是卡住还是断开"));
		HeartbeatListener.Reset();
		return 0;
	}
	HeartbeatTickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&HeartbeatTick), 0.0f);
	return Port;
}

void UELinkHeartbeat::Stop()
{
	if (HeartbeatTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickHandle);
		HeartbeatTickHandle.Reset();
	}
	// FTcpListener 析构时停掉并等监听线程退出
	HeartbeatListener.Reset();
}
