#include "build.h"
#include "xenonLibNatives.h"
#include "xenonThread.h"
#include "xenonKernel.h"
#include "xenonCPU.h"
#include "xenonCPUDevice.h"
#include "xenonInplaceExecution.h"

#include "..\..\..\launcher\backend\nativeMemory.h"
#include "..\..\..\launcher\backend\nativeKernel.h"
#include "..\..\..\launcher\backend\native.h"
#include "..\..\..\launcher\backend\runtimeCodeExecutor.h"
#include "..\..\..\launcher\backend\launcherCommandline.h"
#include "..\..\..\launcher\backend\runtimeTraceWriter.h"

namespace xenon
{
	//-----------------------------------------------------------------------------

	static uint32 ConvWaitResult( native::WaitResult result )
	{
		switch (result)
		{
			case native::WaitResult::Success: return xnative::X_STATUS_SUCCESS;
			case native::WaitResult::IOCompletion:return xnative::X_STATUS_USER_APC;
			case native::WaitResult::Timeout: return xnative::X_STATUS_TIMEOUT;
		}

		return xnative::X_STATUS_ABANDONED_WAIT_0;
	}

	static uint32 TimeoutTicksToMs(int64 timeout_ticks)
	{
		if (timeout_ticks > 0)
		{
			DEBUG_CHECK(!"Invalid time base");
			return 0;
		}
		else if (timeout_ticks < 0)
		{
			return (uint32)(-timeout_ticks / 10000); // Ticks -> MS
		}

		return 0;
	}

	//-----------------------------------------------------------------------------

	IKernelObject::IKernelObject(Kernel* kernel, const KernelObjectType type, const char* name)
		: m_type(type)
		, m_name(name)
		, m_kernel(kernel)
		, m_index(0)
	{
		m_kernel->AllocIndex(this, m_index);
		DEBUG_CHECK(m_index != 0);
	}

	IKernelObject::~IKernelObject()
	{
		m_kernel->ReleaseIndex(this, m_index);
		m_kernel = nullptr;
		m_index = 0;
	}

	void IKernelObject::SetObjectName(const char* name)
	{
		if (m_name != name)
			m_name = name;
	}

	const uint32 IKernelObject::GetHandle() const
	{
		uint32 id = (uint32)m_type << 24;
		id |= m_index;
		return id;
	}

	void IKernelObject::SetNativePointer(void* nativePtr)
	{
		xnative::XDISPATCH_HEADER* headerBE = (xnative::XDISPATCH_HEADER*) nativePtr;

		xnative::XDISPATCH_HEADER header;
		header.TypeFlags = mem::load<uint32>(&headerBE->TypeFlags);
		header.SignalState = mem::load<uint32>(&headerBE->SignalState);
		header.WaitListFLink = mem::load<uint32>(&headerBE->WaitListFLink);
		header.WaitListBLink = mem::load<uint32>(&headerBE->WaitListBLink);
		DEBUG_CHECK(!(header.WaitListBLink & 0x1));

		// Stash pointer in struct
		uint64 objectPtr = reinterpret_cast<uint64>(this);
		objectPtr |= 0x1;
		mem::store<uint32>(&headerBE->WaitListFLink, (uint32)(objectPtr >> 32));
		mem::store<uint32>(&headerBE->WaitListBLink, (uint32)(objectPtr >> 0));
	}

	//-----------------------------------------------------------------------------

	IKernelObjectRefCounted::IKernelObjectRefCounted(Kernel* kernel, const KernelObjectType type, const char* name)
		: IKernelObject(kernel, type, name)
		, m_refCount(1)
	{
	}

	IKernelObjectRefCounted::~IKernelObjectRefCounted()
	{
		//DEBUG_CHECK(m_refCount == 0);
	}

	void IKernelObjectRefCounted::AddRef()
	{
		DEBUG_CHECK(m_refCount > 0);
		InterlockedIncrement(&m_refCount);
	}

	void IKernelObjectRefCounted::Release()
	{
		if (0 == InterlockedDecrement(&m_refCount))
		{
			delete this;
		}
	}

	//-----------------------------------------------------------------------------

	IKernelWaitObject::IKernelWaitObject(Kernel* kernel, const KernelObjectType type, const char* name)
		: IKernelObjectRefCounted(kernel, type, name)
	{}

	//-----------------------------------------------------------------------------

	KernelList::KernelList()
		: m_headAddr(0)
	{
	}

	void KernelList::Insert(const uint32 listEntryPtr)
	{
		mem::storeAddr<uint32>(listEntryPtr + 0, m_headAddr);
		mem::storeAddr<uint32>(listEntryPtr + 4, m_headAddr);

		if (m_headAddr != INVALID)
			mem::storeAddr<uint32>(m_headAddr + 4, listEntryPtr);
	}

	bool KernelList::IsQueued(const uint32 listEntryPtr)
	{
		const uint32 flink = mem::loadAddr<uint32>(listEntryPtr + 0);
		const uint32 blink = mem::loadAddr<uint32>(listEntryPtr + 4);
		return (m_headAddr == listEntryPtr) || (flink != 0) || (blink != 0);
	}

	void KernelList::Remove(const uint32 listEntryPtr)
	{
		const uint32 flink = mem::loadAddr<uint32>(listEntryPtr + 0);
		const uint32 blink = mem::loadAddr<uint32>(listEntryPtr + 4);

		if (listEntryPtr == m_headAddr)
		{
			m_headAddr = flink;

			if (flink != 0)
				mem::storeAddr<uint32>(flink + 4, 0);
		}
		else
		{
			if (blink != 0)
				mem::storeAddr<uint32>(blink + 0, flink);

			if (flink != 0)
				mem::storeAddr<uint32>(flink + 4, blink);
		}

		mem::storeAddr<uint32>(listEntryPtr + 0, 0);
		mem::storeAddr<uint32>(listEntryPtr + 4, 0);
	}

	const uint32 KernelList::Pop()
	{
		if (!m_headAddr)
			return 0;

		uint32 ptr = m_headAddr;
		Remove(ptr);
		DEBUG_CHECK(m_headAddr != ptr);
		return ptr;
	}

	bool KernelList::HasPending() const
	{
		return (m_headAddr != INVALID) && (m_headAddr != NULL);
	}

	//-----------------------------------------------------------------------------

	KernelDispatcher::KernelDispatcher(native::ICriticalSection* lock)
		: m_list(new KernelList())
		, m_lock(lock)
	{
	}

	KernelDispatcher::~KernelDispatcher()
	{
		delete m_lock;
		delete m_list;
	}

	void KernelDispatcher::Lock()
	{
		m_lock->Acquire();
	}

	void KernelDispatcher::Unlock()
	{
		m_lock->Release();
	}

	//-----------------------------------------------------------------------------

	KernelStackMemory::KernelStackMemory(Kernel* kernel, const uint32 size)
		: IKernelObject(kernel, KernelObjectType::Stack, "Stack")
		, m_base(nullptr)
		, m_top(nullptr)
		, m_memory(&kernel->GetNativeMemory())
		, m_size(size)
	{
		// alloc stack
		m_base = m_memory->VirtualAlloc(NULL, m_size, native::IMemory::eAlloc_Top | native::IMemory::eAlloc_Reserve | native::IMemory::eAlloc_Commit, native::IMemory::eFlags_ReadWrite);
		DEBUG_CHECK(m_base != nullptr);

		// setup
		m_top = (uint8*)m_base + size - 0xB0;
		m_size = size;

		// cleanup stack with pattern
		memset(m_base, 0xCD, size);
	}

	KernelStackMemory::~KernelStackMemory()
	{
		uint32 freedSize = 0;
		m_memory->VirtualFree(m_base, m_size, native::IMemory::eAlloc_Decomit | native::IMemory::eAlloc_Release, freedSize);

		m_base = nullptr;
		m_memory = nullptr;
		m_top = nullptr;
		m_size = 0;
	}

	//-----------------------------------------------------------------------------

	KernelThreadMemory::KernelThreadMemory(Kernel* kernel, const uint32 stackSize, const uint32 threadId, const uint64 entryPoint, const uint32 cpuIndex)
		: IKernelObject(kernel, KernelObjectType::ThreadBlock, "ThreadBlock")
		, m_stack(kernel, stackSize)
	{
		// total size to allocate
		const uint32 totalThreaDataSize = (THREAD_DATA_SIZE + PRC_DATA_SIZE + TLS_COUNT + SCRATCH_SIZE) * sizeof(uint32);

		// alloc stack
		m_block = kernel->GetNativeMemory().VirtualAlloc(NULL, totalThreaDataSize, native::IMemory::eAlloc_Top | native::IMemory::eAlloc_Reserve | native::IMemory::eAlloc_Commit, native::IMemory::eFlags_ReadWrite);
		memset(m_block, 0, totalThreaDataSize);
		DEBUG_CHECK(m_block != nullptr);

		// prepare data buffers
		const auto* base = ((uint32*)m_block);
		m_tlsDataAddr = (uint32)&base[0];
		m_prcAddr = (uint32)&base[TLS_COUNT];
		m_threadDataAddr = (uint32)&base[TLS_COUNT + PRC_DATA_SIZE];
		m_scratchAddr = (uint32)&base[TLS_COUNT + PRC_DATA_SIZE + THREAD_DATA_SIZE];

		// setup prc data block
		mem::storeAddr<uint32>(m_prcAddr + 0x000, m_tlsDataAddr); // tls address
		mem::storeAddr<uint32>(m_prcAddr + 0x030, m_prcAddr); // prc address
		mem::storeAddr<uint32>(m_prcAddr + 0x070, (uint32)m_stack.GetTop()); // stack to
		mem::storeAddr<uint32>(m_prcAddr + 0x070, (uint32)m_stack.GetBase()); // stack base
		mem::storeAddr<uint32>(m_prcAddr + 0x100, m_threadDataAddr); // thread data block
		mem::storeAddr<uint8>(m_prcAddr + 0x10C, cpuIndex); // cpu flag
		mem::storeAddr<uint32>(m_prcAddr + 0x150, 1); // dpc flag (guess)

		// setup internal descriptor layout
		// xnative::XDISPATCH_HEADER
		mem::storeAddr<uint32>(m_threadDataAddr + 0x000, 6); // ThreadObject
		mem::storeAddr<uint32>(m_threadDataAddr + 0x008, m_threadDataAddr + 0x008); // list pointer
		mem::storeAddr<uint32>(m_threadDataAddr + 0x00C, m_threadDataAddr + 0x008);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x010, m_threadDataAddr + 0x010); // list pointer

		mem::storeAddr<uint32>(m_threadDataAddr + 0x014, m_threadDataAddr + 0x010);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x040, m_threadDataAddr + 0x020); // list pointer
		mem::storeAddr<uint32>(m_threadDataAddr + 0x044, m_threadDataAddr + 0x020);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x048, m_threadDataAddr + 0x000);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x04C, m_threadDataAddr + 0x018);
		mem::storeAddr<uint16>(m_threadDataAddr + 0x054, 0x102);
		mem::storeAddr<uint16>(m_threadDataAddr + 0x056, 0x1);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x05C, (uint32)m_stack.GetTop());
		mem::storeAddr<uint32>(m_threadDataAddr + 0x060, (uint32)m_stack.GetBase());
		mem::storeAddr<uint32>(m_threadDataAddr + 0x068, m_tlsDataAddr);
		mem::storeAddr<uint8>(m_threadDataAddr + 0x06C, 0);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x074, m_threadDataAddr + 0x074);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x078, m_threadDataAddr + 0x074);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x07C, m_threadDataAddr + 0x07C);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x080, m_threadDataAddr + 0x07C);
		mem::storeAddr<uint8>(m_threadDataAddr + 0x08B, 1);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x09C, 0xFDFFD7FF);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x0D0, (uint32)m_stack.GetTop());
		mem::storeAddr<uint32>(m_threadDataAddr + 0x144, m_threadDataAddr + 0x144);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x148, m_threadDataAddr + 0x144);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x14C, (uint32)GetIndex());
		mem::storeAddr<uint32>(m_threadDataAddr + 0x150, (uint32)entryPoint);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x154, m_threadDataAddr + 0x154);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x158, m_threadDataAddr + 0x154);
		mem::storeAddr<uint32>(m_threadDataAddr + 0x160, 0); // last error
		mem::storeAddr<uint32>(m_threadDataAddr + 0x16C, 0); // creation flags
		mem::storeAddr<uint32>(m_threadDataAddr + 0x17C, 1);

		FILETIME t;
		GetSystemTimeAsFileTime(&t);
		mem::storeAddr<uint64>(m_threadDataAddr + 0x130, ((uint64)t.dwHighDateTime << 32) | t.dwLowDateTime);
	}

	KernelThreadMemory::~KernelThreadMemory()
	{		
		uint32 freedSize = 0;
		GetOwner()->GetNativeMemory().VirtualFree(m_block, 0, native::IMemory::eAlloc_Decomit | native::IMemory::eAlloc_Release, freedSize);

		m_block = nullptr;
		m_prcAddr = 0;
		m_threadDataAddr = 0;
		m_scratchAddr = 0;
		m_tlsDataAddr = 0;
	}

	//-----------------------------------------------------------------------------

	KernelTLS::KernelTLS(Kernel* kernel)
		: IKernelObject(kernel, KernelObjectType::TLS, "TLS")
	{
		memset(&m_entries, 0, sizeof(m_entries));
	}

	KernelTLS::~KernelTLS()
	{
	}

	void KernelTLS::Set(const uint32 entryIndex, const uint64 value)
	{
		if (entryIndex < MAX_ENTRIES)
			m_entries[entryIndex] = value;
	}

	uint64 KernelTLS::Get(const uint32 entryIndex) const
	{
		if (entryIndex < MAX_ENTRIES)
			return m_entries[entryIndex];

		return 0;
	}

	//-----------------------------------------------------------------------------

	KernelCriticalSection::KernelCriticalSection(Kernel* kernel, native::ICriticalSection* nativeObject)
		: IKernelObject(kernel, KernelObjectType::CriticalSection, "CriticalSection")
		, m_lock(nativeObject)
		, m_isLocked(false)
		, m_spinCount(0)
	{
	}

	KernelCriticalSection::~KernelCriticalSection()
	{
		delete m_lock;
		m_lock = nullptr;
	}

	void KernelCriticalSection::Enter()
	{
		m_isLocked = true;
		m_lock->Acquire();
	}

	void KernelCriticalSection::Leave()
	{
		m_lock->Release();
		m_isLocked = false;
	}

	//-----------------------------------------------------------------------------

	KernelEvent::KernelEvent(Kernel* kernel, native::IEvent* nativeEvent, const char* name)
		: IKernelWaitObject(kernel, KernelObjectType::Event, name)
		, m_event(nativeEvent)
	{
		//GLog.Log("****CREATE: %d", GetIndex());
	}	

	KernelEvent::~KernelEvent()
	{
		delete m_event;
		m_event = nullptr;
	}

	uint32 KernelEvent::Set(uint32 priority_increment, bool wait)
	{
		//GLog.Log("****SET: %d", GetIndex());
		return m_event->Set();
	}

	uint32 KernelEvent::Pulse(uint32 priority_increment, bool wait)
	{
		return m_event->Pulse();
	}

	uint32 KernelEvent::Reset()
	{
		//GLog.Log("****RESET: %d", GetIndex());
		return m_event->Reset();
	}

	uint32 KernelEvent::Wait(const uint32 waitReason, const uint32 processorMode, const bool alertable, const int64* optTimeout)
	{
		const auto timeoutValue = optTimeout ? TimeoutTicksToMs(*optTimeout) : native::TimeoutInfinite;
		const auto result = m_event->Wait(timeoutValue, alertable);
		//GLog.Log("****WAIT: %d, %d", GetIndex(), result);
		return ConvWaitResult(result);
	}

	//-----------------------------------------------------------------------------

	Kernel::Kernel(const native::Systems& system, const launcher::Commandline& cmdline)
		: m_nativeKernel( system.m_kernel )
		, m_nativeMemory( system.m_memory )
		, m_maxObjectIndex(1)
		, m_numFreeIndices(0)
		, m_exitRequested( false )
	{
		m_lock = m_nativeKernel->CreateCriticalSection();
		m_interruptLock = m_nativeKernel->CreateCriticalSection();
		m_tlsLock = m_nativeKernel->CreateCriticalSection();
		m_threadLock = m_nativeKernel->CreateCriticalSection();

		memset( &m_objects, 0, sizeof(m_objects) );
		memset( &m_freeIndices, 0, sizeof(m_freeIndices) );

		for (uint32 i = 0; i < MAX_TLS; ++i)
			m_tlsFreeEntries[i] = true;

		// tracing enabled ?
		if (cmdline.HasOption("tracefile"))
		{
			m_traceFileRootName = cmdline.GetOptionValueW("tracefile");
			if (!m_traceFileRootName.empty())
			{
				GLog.Warn("Kernel: Trace file enable, all traces will be outputed as '%ls'", m_traceFileRootName.c_str());
			}
		}
	}

	Kernel::~Kernel()
	{
		if (!m_threads.empty())
		{
			GLog.Log("Kernel: There are still %d threads running, stopping them", m_threads.size());

			for (auto* ptr : m_threads)
				delete ptr;
		}

		delete m_lock;
		delete m_interruptLock;
		delete m_threadLock;
		delete m_tlsLock;
	}

	void Kernel::AllocIndex(IKernelObject* object, uint32& outIndex)
	{
		DEBUG_CHECK(object != nullptr);
		DEBUG_CHECK(outIndex == 0);

		m_lock->Acquire();

		// get from list
		if (m_maxObjectIndex < MAX_OBJECT)
		{
			outIndex = m_maxObjectIndex++;
		}
		else if (m_numFreeIndices > 0)
		{
			outIndex = m_freeIndices[m_numFreeIndices - 1];
			m_numFreeIndices -= 1;
		}

		DEBUG_CHECK(m_objects[outIndex] == NULL);
		m_objects[outIndex] = object;

		m_lock->Release();
	}

	void Kernel::ReleaseIndex(IKernelObject* object, uint32& outIndex)
	{
		DEBUG_CHECK(object != nullptr);
		DEBUG_CHECK(outIndex != 0);

		m_lock->Acquire();

		DEBUG_CHECK(m_objects[outIndex] == object);
		m_objects[outIndex] = nullptr;

		m_freeIndices[m_numFreeIndices] = outIndex;
		outIndex = 0;

		m_numFreeIndices += 1;

		m_lock->Release();
	}

	IKernelObject* Kernel::ResolveAny(const uint32 handle)
	{
		// no object
		if (!handle)
			return nullptr;

		// special cases
		if (handle == 0xFFFFFFFF)
		{
			GLog.Err("Kernel: CurrentProcess, WTF? ");
			return 0;
		}
		else if (handle == 0xFFFFFFFE)
		{
			return KernelThread::GetCurrentThread();
		}

		// generic case - get the id
		const uint32 index = handle & (MAX_OBJECT - 1);
		if (!index)
		{
			GLog.Err("Kernel: unresolved object, ID=%08X", handle);
			return nullptr;
		}

		return m_objects[index];
	}

	IKernelObject* Kernel::ResolveHandle(const uint32 handle, KernelObjectType requestedType)
	{
		// no object
		if (!handle)
			return nullptr;

		// special cases
		if (handle == 0xFFFFFFFF)
		{
			GLog.Err("Kernel: CurrentProcess, WTF? ");
			return 0;
		}
		else if (handle == 0xFFFFFFFE)
		{
			if (requestedType != KernelObjectType::Thread)
				return nullptr;

			return KernelThread::GetCurrentThread();
		}

		// generic case - get the id
		const uint32 index = handle & (MAX_OBJECT - 1);
		if (!index)
		{
			GLog.Err("Kernel: unresolved handle, ID=%08X", handle);
			return nullptr;
		}

		IKernelObject* object = m_objects[index];
		if (!object)
		{
			GLog.Err("Kernel: unresolved object, ID=%08X", handle);
			cpu::UnhandledSystemError("Kernel object not found");
			return nullptr;
		}

		const KernelObjectType type = (KernelObjectType)(handle >> 24);
		if (object->GetType() != type)
		{
			GLog.Err("Kernel: unresolved object, ID=%08X, type=%d/%d", handle, type, object->GetType());
			cpu::UnhandledSystemError("Kernel object mismatch");
			return nullptr;
		}

		if ((requestedType != KernelObjectType::Unknown) && (requestedType != type))
		{
			GLog.Err("Kernel: object type mismatch, ID=%08X, actual type=%d, requested type=%d", handle, type, requestedType);
			return nullptr;
		}

		return object;
	}

	IKernelObject* Kernel::ResolveObject(void* nativePtr, NativeKernelObjectType requestedType, const bool allowCreate)
	{
		// Unfortunately the XDK seems to inline some KeInitialize calls, meaning
		// we never see it and just randomly start getting passed events/timers/etc.
		// Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
		// we don't have to worry about PPC code poking the struct. Because of that,
		// we init on first use, store our pointer in the struct, and dereference it
		// each time.
		// We identify this by checking the low bit of wait_list_blink - if it's 1,
		// we have already put our pointer in there.
		xnative::XDISPATCH_HEADER* headerBE = (xnative::XDISPATCH_HEADER*) nativePtr;

		// get true header
		xnative::XDISPATCH_HEADER header;
		header.TypeFlags = mem::load<uint32>(&headerBE->TypeFlags);
		header.SignalState = mem::load<uint32>(&headerBE->SignalState);
		header.WaitListFLink = mem::load<uint32>(&headerBE->WaitListFLink);
		header.WaitListBLink = mem::load<uint32>(&headerBE->WaitListBLink);

		// use the existing type
		if (requestedType == NativeKernelObjectType::Unknown)
		{
			requestedType = (NativeKernelObjectType)(header.TypeFlags & 0xFF);
		}
		else
		{
			const auto actualType = (NativeKernelObjectType)(header.TypeFlags & 0xFF);
			if (actualType != requestedType)
			{
				GLog.Err("Kernel: object type mismatch, ptr=%08X, actual type=%d, requested type=%d", (uint32)nativePtr, actualType, requestedType);
				return nullptr;
			}
		}

		// extract existing pointer
		if (header.WaitListBLink & 0x1)
		{
			// already initialized
			uint64 objectPtr = ((uint64)header.WaitListFLink) << 32;
			objectPtr |= header.WaitListBLink & ~1;

			return (IKernelObject*)objectPtr;
		}

		// create object
		if (!allowCreate)
			return nullptr;

		// First use from native struct, create new.
		// http://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
		IKernelObject* object = NULL;
		switch (requestedType)
		{
		case NativeKernelObjectType::EventNotificationObject:
		case NativeKernelObjectType::EventSynchronizationObject:
		{
			const bool manualReset = (header.TypeFlags >> 24) == 0;
			const bool initialState = header.SignalState ? true : false;
			auto* nativeEvent = m_nativeKernel->CreateEvent( manualReset, initialState );
			object = new KernelEvent(this, nativeEvent, "NativeEvent");
			break;
		}

		case NativeKernelObjectType::MutantObject:
		{
			GLog.Err("Kernel: GetObject - trying to initialize mutant");
			//object = new CMutant(nativePtr, header);
			break;
		}

		case NativeKernelObjectType::SemaphoreObject:
		{
			GLog.Err("Kernel: GetObject - trying to initialize semaphore");
			//object = new CSemaphore(nativePtr, header);
			break;
		}

		case NativeKernelObjectType::ProcessObject:
		case NativeKernelObjectType::QueueObject:
		case NativeKernelObjectType::ThreadObject:
		case NativeKernelObjectType::GateObject:
		case NativeKernelObjectType::TimerNotificationObject:
		case NativeKernelObjectType::TimerSynchronizationObject:
		case NativeKernelObjectType::ApcObject:
		case NativeKernelObjectType::DpcObject:
		case NativeKernelObjectType::DeviceQueueObject:
		case NativeKernelObjectType::EventPairObject:
		case NativeKernelObjectType::InterruptObject:
		case NativeKernelObjectType::ProfileObject:
		case NativeKernelObjectType::ThreadedDpcObject:
		default:
		{
			GLog.Err("Kernel: Unknown/uninitialized object type: %d", requestedType);
			DebugBreak();
			return NULL;
		}
		}

		// Stash pointer in struct
		uint64 objectPtr = reinterpret_cast<uint64>(object);
		objectPtr |= 0x1;
		mem::store<uint32>(&headerBE->WaitListFLink, (uint32)(objectPtr >> 32));
		mem::store<uint32>(&headerBE->WaitListBLink, (uint32)(objectPtr >> 0));

		// return mapped object
		return object;
	}

	void Kernel::ExecuteInterrupt(const uint32 cpuIndex, const uint32 callback, const uint64* args, const uint32 numArgs, const bool trace)
	{
		m_interruptLock->Acquire();

		static uint32 s_nextThreadID = 10000;

		// setup execution context
		InplaceExecutionParams cpuExecutionParams;
		cpuExecutionParams.m_stackSize = 32 << 10;
		cpuExecutionParams.m_entryPoint = callback;
		cpuExecutionParams.m_cpu = cpuIndex;
		cpuExecutionParams.m_threadId = s_nextThreadID++;
		cpuExecutionParams.m_args[0] = numArgs >= 1 ? args[0] : 0;
		cpuExecutionParams.m_args[1] = numArgs >= 2 ? args[1] : 0;

		// execute code
		InplaceExecution cpuExecution(this, cpuExecutionParams);
		cpuExecution.Execute(trace);

		m_interruptLock->Release();
	}

	uint32 Kernel::AllocTLSIndex()
	{
		m_tlsLock->Acquire();

		int32 freeIndex = -1;
		for (uint32 i = 0; i < MAX_TLS; ++i)
		{
			if (m_tlsFreeEntries[i])
			{
				freeIndex = i;
				break;
			}
		}

		DEBUG_CHECK(freeIndex != -1);

		m_tlsFreeEntries[freeIndex] = false;

		m_tlsLock->Release();

		return freeIndex;
	}

	void Kernel::FreeTLSIndex(const uint32 index)
	{
		m_tlsLock->Acquire();

		DEBUG_CHECK(m_tlsFreeEntries[index] == false);
		m_tlsFreeEntries[index] = true;

		m_tlsLock->Release();
	}

	void Kernel::SetCode(const runtime::CodeTable* code)
	{
		m_codeTable = code;
	}

	bool Kernel::AdvanceThreads()
	{
		bool hasRunningThreads = false;
		bool hasCrashedThreads = false;

		// locked access
		{
			m_threadLock->Acquire();

			// process the thread update
			for (auto it = m_threads.begin(); it != m_threads.end(); )
			{
				auto* thread = *it;

				if (thread->HasCrashed())
				{
					GLog.Log("Kernel: Thread '%s' (ID=%d) has crashed", thread->GetName(), thread->GetIndex());
					hasCrashedThreads = true;
				}

				if (thread->HasStopped())
				{
					GLog.Log("Process: Thread '%s' (ID=%d) removed from thread list", thread->GetName(), thread->GetIndex());

					delete thread;
					it = m_threads.erase(it);
				}
				else
				{
					hasRunningThreads = true;
					++it;
				}
			}

			m_threadLock->Release();
		}

		// all threads exited
		if (!hasRunningThreads)
		{
			GLog.Log("Process: All threads exited. Killing process.");
			return false;
		}

		// thread crashed
		if (hasCrashedThreads)
		{
			GLog.Log("Process: A thread crashed during program execution. Killing process.");
			return false;
		}

		// keep running until exit is request
		return !m_exitRequested;
	}

	static std::atomic<int> GTraceFileIndex = 1;

	runtime::TraceWriter* Kernel::CreateThreadTraceWriter()
	{
		// tracing disabled
		if (m_traceFileRootName.empty())
			return nullptr;

		// format a name
		wchar_t buf[16];
		const int traceIndex = GTraceFileIndex++;
		wsprintf(buf, L".%d.trace", traceIndex);
		std::wstring traceFileName = m_traceFileRootName;
		traceFileName += buf;

		// create the trace file
		return runtime::TraceWriter::CreateTrace(CPU_RegisterBankInfo::GetInstance(), traceFileName);
	}

	//---

	KernelThread* Kernel::CreateThread(const KernelThreadParams& params)
	{
		auto* traceFile = CreateThreadTraceWriter();
		auto* thread = new KernelThread(this, traceFile, params);

		m_threadLock->Acquire();
		m_threads.push_back(thread);
		m_threadLock->Release();

		// resume execution
		if (!params.m_suspended)
			thread->Resume();

		return thread;
	}

	KernelEvent* Kernel::CreateEvent(bool initialState, bool manualReset)
	{
		auto* nativeEvent = m_nativeKernel->CreateEvent(manualReset, initialState );
		return new KernelEvent(this, nativeEvent);
	}

	KernelCriticalSection* Kernel::CreateCriticalSection()
	{
		auto* nativeCriticalSection = m_nativeKernel->CreateCriticalSection();
		return new KernelCriticalSection(this, nativeCriticalSection);
	}

	//----

	uint32 KernelThread::Wait(const uint32 waitReason, const uint32 processorMode, const bool alertable, const int64* optTimeout)
	{
		const auto timeoutValue = optTimeout ? TimeoutTicksToMs(*optTimeout) : native::TimeoutInfinite;
		const auto result = m_nativeThread->Wait(timeoutValue, alertable);
		return ConvWaitResult(result);
	}

	//----

} // xenon