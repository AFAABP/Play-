#include <stddef.h>
#include <stdlib.h>
#include <exception>
#include <boost/filesystem/path.hpp>
#include "PS2OS.h"
#include "Ps2Const.h"
#include "StdStream.h"
#include "PtrMacro.h"
#include "Utils.h"
#include "DMAC.h"
#include "INTC.h"
#include "SIF.h"
#include "ElfFile.h"
#include "COP_SCU.h"
#include "uint128.h"
#include "MIPSAssembler.h"
#include "Profiler.h"
#include "PathUtils.h"
#include "xml/Node.h"
#include "xml/Parser.h"
#include "xml/FilteringNodeIterator.h"
#include "Log.h"
#include "iop/IopBios.h"
#include "StdStreamUtils.h"

// PS2OS Memory Allocation
// Start		End				Description
// 0x80000000	0x80000004		Current Thread ID
// 0x80008000	0x8000A000		DECI2 Handlers
// 0x8000A000	0x8000C000		INTC Handlers
// 0x8000C000	0x8000E000		DMAC Handlers
// 0x8000E000	0x80010000		Semaphores
// 0x80010000	0x80010800		Custom System Call addresses (0x200 entries)
// 0x80011000	0x80020000		Threads
// 0x80020000	0x80030000		Kernel Stack
// 0x80030000	0x80032000		Thread Linked List

// BIOS area
// Start		End				Description
// 0x1FC00004	0x1FC00008		REEXCEPT instruction (for exception reentry) to be changed
// 0x1FC00100	0x1FC00200		Custom System Call handling code
// 0x1FC00200	0x1FC01000		Interrupt Handler
// 0x1FC01000	0x1FC02000		DMAC Interrupt Handler
// 0x1FC02000	0x1FC03000		GS Interrupt Handler
// 0x1FC03000	0x1FC03100		Thread epilogue
// 0x1FC03100	0x1FC03200		Wait Thread Proc

#define BIOS_ADDRESS_BASE			0x1FC00000
#define BIOS_ADDRESS_WAITTHREADPROC	0x1FC03100

#define CONFIGPATH		"./config/"
#define PATCHESFILENAME	"patches.xml"
#define LOG_NAME		("ps2os")

#define THREAD_INIT_QUOTA			(15)

#define SYSCALL_NAME_LOADEXECPS2			"osLoadExecPS2"
#define SYSCALL_NAME_ADDINTCHANDLER			"osAddIntcHandler"
#define SYSCALL_NAME_ENABLEINTC				"osEnableIntc"
#define SYSCALL_NAME_CREATETHREAD			"osCreateThread"
#define SYSCALL_NAME_STARTTHREAD			"osStartThread"
#define SYSCALL_NAME_ICHANGETHREADPRIORITY	"osiChangeThreadPriority"
#define SYSCALL_NAME_ROTATETHREADREADYQUEUE	"osRotateThreadReadyQueue"
#define SYSCALL_NAME_GETTHREADID			"osGetThreadId"
#define SYSCALL_NAME_REFERTHREADSTATUS		"osReferThreadStatus"
#define SYSCALL_NAME_IREFERTHREADSTATUS		"osiReferThreadStatus"
#define SYSCALL_NAME_SLEEPTHREAD			"osSleepThread"
#define SYSCALL_NAME_WAKEUPTHREAD			"osWakeupThread"
#define SYSCALL_NAME_IWAKEUPTHREAD			"osiWakeupThread"
#define SYSCALL_NAME_SUSPENDTHREAD			"osSuspendThread"
#define SYSCALL_NAME_RESUMETHREAD			"osResumeThread"
#define SYSCALL_NAME_ENDOFHEAP				"osEndOfHeap"
#define SYSCALL_NAME_CREATESEMA				"osCreateSema"
#define SYSCALL_NAME_DELETESEMA				"osDeleteSema"
#define SYSCALL_NAME_SIGNALSEMA				"osSignalSema"
#define SYSCALL_NAME_ISIGNALSEMA			"osiSignalSema"
#define SYSCALL_NAME_WAITSEMA				"osWaitSema"
#define SYSCALL_NAME_POLLSEMA				"osPollSema"
#define SYSCALL_NAME_FLUSHCACHE				"osFlushCache"
#define SYSCALL_NAME_GSGETIMR				"osGsGetIMR"
#define SYSCALL_NAME_GSPUTIMR				"osGsPutIMR"
#define SYSCALL_NAME_SIFDMASTAT				"osSifDmaStat"
#define SYSCALL_NAME_SIFSETDMA				"osSifSetDma"
#define SYSCALL_NAME_SIFSETDCHAIN			"osSifSetDChain"

#ifdef DEBUGGER_INCLUDED

const CPS2OS::SYSCALL_NAME	CPS2OS::g_syscallNames[] =
{
	{	0x0006,		SYSCALL_NAME_LOADEXECPS2			},
	{	0x0010,		SYSCALL_NAME_ADDINTCHANDLER			},
	{	0x0014,		SYSCALL_NAME_ENABLEINTC				},
	{	0x0020,		SYSCALL_NAME_CREATETHREAD			},
	{	0x0022,		SYSCALL_NAME_STARTTHREAD			},
	{	0x002A,		SYSCALL_NAME_ICHANGETHREADPRIORITY	},
	{	0x002B,		SYSCALL_NAME_ROTATETHREADREADYQUEUE	},
	{	0x002F,		SYSCALL_NAME_GETTHREADID			},
	{	0x0030,		SYSCALL_NAME_REFERTHREADSTATUS		},
	{	0x0031,		SYSCALL_NAME_IREFERTHREADSTATUS		},
	{	0x0032,		SYSCALL_NAME_SLEEPTHREAD			},
	{	0x0033,		SYSCALL_NAME_WAKEUPTHREAD			},
	{	0x0034,		SYSCALL_NAME_IWAKEUPTHREAD			},
	{	0x0037,		SYSCALL_NAME_SUSPENDTHREAD			},
	{	0x0039,		SYSCALL_NAME_RESUMETHREAD			},
	{	0x003E,		SYSCALL_NAME_ENDOFHEAP				},
	{	0x0040,		SYSCALL_NAME_CREATESEMA				},
	{	0x0041,		SYSCALL_NAME_DELETESEMA				},
	{	0x0042,		SYSCALL_NAME_SIGNALSEMA				},
	{	0x0043,		SYSCALL_NAME_ISIGNALSEMA			},
	{	0x0044,		SYSCALL_NAME_WAITSEMA				},
	{	0x0045,		SYSCALL_NAME_POLLSEMA				},
	{	0x0064,		SYSCALL_NAME_FLUSHCACHE				},
	{	0x0070,		SYSCALL_NAME_GSGETIMR				},
	{	0x0071,		SYSCALL_NAME_GSPUTIMR				},
	{	0x0076,		SYSCALL_NAME_SIFDMASTAT				},
	{	0x0077,		SYSCALL_NAME_SIFSETDMA				},
	{	0x0078,		SYSCALL_NAME_SIFSETDCHAIN			},
	{	0x0000,		NULL								}
};

#endif

namespace filesystem = boost::filesystem;

CPS2OS::CPS2OS(CMIPS& ee, uint8* ram, uint8* bios, CGSHandler*& gs, CSIF& sif, CIopBios& iopBios) 
: m_ee(ee)
, m_gs(gs)
, m_pELF(NULL)
, m_ram(ram)
, m_bios(bios)
, m_pThreadSchedule(NULL)
, m_sif(sif)
, m_iopBios(iopBios)
{
	Initialize();
}

CPS2OS::~CPS2OS()
{
	Release();
}

void CPS2OS::Initialize()
{
	m_pELF = NULL;

	m_ee.m_State.nGPR[CMIPS::K0].nV[0] = 0x80030000;
	m_ee.m_State.nGPR[CMIPS::K0].nV[1] = 0xFFFFFFFF;

	m_pThreadSchedule = new CRoundRibbon(m_ram + 0x30000, 0x2000);

	m_semaWaitId = -1;
	m_semaWaitCount = 0;
	m_semaWaitCaller = 0;
	m_semaWaitThreadId = -1;
}

void CPS2OS::Release()
{
	UnloadExecutable();
	
	DELETEPTR(m_pThreadSchedule);
}

bool CPS2OS::IsIdle() const
{
	return (GetCurrentThreadId() == m_semaWaitThreadId);
}

void CPS2OS::DumpIntcHandlers()
{
	printf("INTC Handlers Information\r\n");
	printf("-------------------------\r\n");

	for(unsigned int i = 0; i < MAX_INTCHANDLER; i++)
	{
		INTCHANDLER* pHandler = GetIntcHandler(i + 1);
		if(pHandler->nValid == 0) continue;

		printf("ID: %0.2i, Line: %i, Address: 0x%0.8X.\r\n", \
			i + 1,
			pHandler->nCause,
			pHandler->nAddress);
	}
}

void CPS2OS::DumpDmacHandlers()
{
	printf("DMAC Handlers Information\r\n");
	printf("-------------------------\r\n");

	for(unsigned int i = 0; i < MAX_DMACHANDLER; i++)
	{
		DMACHANDLER* pHandler = GetDmacHandler(i + 1);
		if(pHandler->nValid == 0) continue;

		printf("ID: %0.2i, Channel: %i, Address: 0x%0.8X.\r\n", \
			i + 1,
			pHandler->nChannel,
			pHandler->nAddress);
	}
}

void CPS2OS::BootFromFile(const char* sPath)
{
	filesystem::path ExecPath(sPath);
	Framework::CStdStream stream(fopen(ExecPath.string().c_str(), "rb"));
	LoadELF(stream, ExecPath.filename().string().c_str(), ArgumentList());
}

void CPS2OS::BootFromCDROM(const ArgumentList& arguments)
{
	std::string executablePath;
	Iop::CIoman* ioman = m_iopBios.GetIoman();

	{
		uint32 handle = ioman->Open(Iop::Ioman::CDevice::OPEN_FLAG_RDONLY, "cdrom0:SYSTEM.CNF");
		if(static_cast<int32>(handle) < 0)
		{
			throw std::runtime_error("No 'SYSTEM.CNF' file found on the cdrom0 device.");
		}

		{
			Framework::CStream* file(ioman->GetFileStream(handle));
			std::string line;

			Utils::GetLine(file, &line);
			while(!file->IsEOF())
			{
				if(!strncmp(line.c_str(), "BOOT2", 5))
				{
					const char* tempPath = strstr(line.c_str(), "=");
					if(tempPath != NULL)
					{
						tempPath++;
						if(tempPath[0] == ' ') tempPath++;
						executablePath = tempPath;
						break;
					}
				}
				Utils::GetLine(file, &line);
			}
		}

		ioman->Close(handle);
	}

	if(executablePath.length() == 0)
	{
		throw std::runtime_error("Error parsing 'SYSTEM.CNF' for a BOOT2 value.");
	}

	{
		uint32 handle = ioman->Open(Iop::Ioman::CDevice::OPEN_FLAG_RDONLY, executablePath.c_str());
		if(static_cast<int32>(handle) < 0)
		{
			throw std::runtime_error("Couldn't open executable specified in SYSTEM.CNF.");
		}

		try
		{
			const char* executableName = strchr(executablePath.c_str(), ':') + 1;
			if(executableName[0] == '/' || executableName[0] == '\\') executableName++;
			Framework::CStream* file(ioman->GetFileStream(handle));
			LoadELF(*file, executableName, arguments);
		}
		catch(...)
		{

		}
		ioman->Close(handle);
	}
}

CELF* CPS2OS::GetELF()
{
	return m_pELF;
}

const char* CPS2OS::GetExecutableName() const
{
	return m_executableName.c_str();
}

std::pair<uint32, uint32> CPS2OS::GetExecutableRange() const
{
	uint32 nMinAddr = 0xFFFFFFF0;
	uint32 nMaxAddr = 0x00000000;
	const ELFHEADER& header = m_pELF->GetHeader();

	for(unsigned int i = 0; i < header.nProgHeaderCount; i++)
	{
		ELFPROGRAMHEADER* p = m_pELF->GetProgram(i);
		if(p != NULL)
		{
			uint32 end = p->nVAddress + p->nFileSize;
			if(end >= PS2::EE_RAM_SIZE) continue;
			nMinAddr = std::min<uint32>(nMinAddr, p->nVAddress);
			nMaxAddr = std::max<uint32>(nMaxAddr, end);
		}
	}

	return std::pair<uint32, uint32>(nMinAddr, nMaxAddr);
}

BiosDebugModuleInfoArray CPS2OS::GetModuleInfos() const
{
	BiosDebugModuleInfoArray result;

	if(m_pELF)
	{
		auto executableRange = GetExecutableRange();

		BIOS_DEBUG_MODULE_INFO module;
		module.name		= m_executableName;
		module.begin	= executableRange.first;
		module.end		= executableRange.second;
		module.param	= m_pELF;
		result.push_back(module);
	}

	return result;
}

BiosDebugThreadInfoArray CPS2OS::GetThreadInfos() const
{
	BiosDebugThreadInfoArray threadInfos;

	CRoundRibbon::ITERATOR threadIterator(m_pThreadSchedule);

	for(threadIterator = m_pThreadSchedule->Begin(); 
		!threadIterator.IsEnd(); threadIterator++)
	{
		auto thread = GetThread(threadIterator.GetValue());
		THREADCONTEXT* threadContext = reinterpret_cast<THREADCONTEXT*>(m_ram + thread->nContextPtr);

		BIOS_DEBUG_THREAD_INFO threadInfo;
		threadInfo.id			= threadIterator.GetValue();
		threadInfo.priority		= thread->nPriority;
		if(GetCurrentThreadId() == threadIterator.GetValue())
		{
			threadInfo.pc = m_ee.m_State.nPC;
			threadInfo.ra = m_ee.m_State.nGPR[CMIPS::RA].nV0;
			threadInfo.sp = m_ee.m_State.nGPR[CMIPS::SP].nV0;
		}
		else
		{
			threadInfo.pc = thread->nEPC;
			threadInfo.ra = threadContext->nGPR[CMIPS::RA].nV0;
			threadInfo.sp = threadContext->nGPR[CMIPS::SP].nV0;
		}

		switch(thread->nStatus)
		{
		case THREAD_RUNNING:
			threadInfo.stateDescription = "Running";
			break;
		case THREAD_SLEEPING:
			threadInfo.stateDescription = "Sleeping";
			break;
		case THREAD_WAITING:
			threadInfo.stateDescription = "Waiting (Semaphore: " + boost::lexical_cast<std::string>(thread->nSemaWait) + ")";
			break;
		case THREAD_SUSPENDED:
			threadInfo.stateDescription = "Suspended";
			break;
		case THREAD_SUSPENDED_SLEEPING:
			threadInfo.stateDescription = "Suspended+Sleeping";
			break;
		case THREAD_SUSPENDED_WAITING:
			threadInfo.stateDescription = "Suspended+Waiting (Semaphore: " + boost::lexical_cast<std::string>(thread->nSemaWait) + ")";
			break;
		case THREAD_ZOMBIE:
			threadInfo.stateDescription = "Zombie";
			break;
		default:
			threadInfo.stateDescription = "Unknown";
			break;
		}

		threadInfos.push_back(threadInfo);
	}

	return threadInfos;
}

void CPS2OS::LoadELF(Framework::CStream& stream, const char* sExecName, const ArgumentList& arguments)
{
	CELF* pELF(new CElfFile(stream));

	const ELFHEADER& header = pELF->GetHeader();

	//Check for MIPS CPU
	if(header.nCPU != 8)
	{
		DELETEPTR(pELF);
		throw std::runtime_error("Invalid target CPU. Must be MIPS.");
	}

	if(header.nType != 2)
	{
		DELETEPTR(pELF);
		throw std::runtime_error("Not an executable ELF file.");
	}
	
	UnloadExecutable();

	m_pELF = pELF;

	m_executableName = sExecName;
	m_currentArguments = arguments;

	LoadExecutableInternal();
	ApplyPatches();

	OnExecutableChange();

	printf("PS2OS: Loaded '%s' executable file.\r\n", sExecName);
}

void CPS2OS::LoadExecutableInternal()
{
	//Copy program in main RAM
	const ELFHEADER& header = m_pELF->GetHeader();
	for(unsigned int i = 0; i < header.nProgHeaderCount; i++)
	{
		ELFPROGRAMHEADER* p = m_pELF->GetProgram(i);
		if(p != NULL)
		{
			memcpy(m_ram + p->nVAddress, m_pELF->GetContent() + p->nOffset, p->nFileSize);
		}
	}

	m_ee.m_State.nPC = header.nEntryPoint;
	
	*(uint32*)&m_bios[0x00000004] = 0x0000001D;
	AssembleCustomSyscallHandler();
	AssembleInterruptHandler();
	AssembleDmacHandler();
	AssembleIntcHandler();
	AssembleThreadEpilog();
	AssembleWaitThreadProc();
	CreateWaitThread();

#ifdef DEBUGGER_INCLUDED
	std::pair<uint32, uint32> executableRange = GetExecutableRange();
	uint32 nMinAddr = executableRange.first;
	uint32 nMaxAddr = executableRange.second & ~0x03;

	m_ee.m_pAnalysis->Clear();
	m_ee.m_pAnalysis->Analyse(nMinAddr, nMaxAddr, header.nEntryPoint);

	//Tag system calls
	for(uint32 address = nMinAddr; address < nMaxAddr; address += 4)
	{
		//Check for SYSCALL opcode
		uint32 opcode = *reinterpret_cast<uint32*>(m_ram + address);
		if(opcode == 0x0000000C)
		{
			//Check the opcode before and after it
			uint32 addiu	= *reinterpret_cast<uint32*>(m_ram + address - 4);
			uint32 jr		= *reinterpret_cast<uint32*>(m_ram + address + 4);
			if(
				(jr == 0x03E00008) && 
				(addiu & 0xFFFF0000) == 0x24030000
				)
			{
				//We have it!
				int16 syscallId = static_cast<int16>(addiu);
				if(syscallId & 0x8000)
				{
					syscallId = 0 - syscallId;
				}
				char syscallName[256];
				int syscallNameIndex = -1;
				for(int i = 0; g_syscallNames[i].name != NULL; i++)
				{
					if(g_syscallNames[i].id == syscallId)
					{
						syscallNameIndex = i;
						break;
					}
				}
				if(syscallNameIndex != -1)
				{
					strncpy(syscallName, g_syscallNames[syscallNameIndex].name, 256);
				}
				else
				{
					sprintf(syscallName, "syscall_%0.4X", syscallId);
				}
				m_ee.m_Functions.InsertTag(address - 4, syscallName);
			}
		}
	}

#endif
}

void CPS2OS::UnloadExecutable()
{
	if(m_pELF == NULL) return;

	OnExecutableUnloading();

	DELETEPTR(m_pELF);
}

uint32 CPS2OS::LoadExecutable(const char* path, const char* section)
{
	auto ioman = m_iopBios.GetIoman();

	uint32 handle = ioman->Open(Iop::Ioman::CDevice::OPEN_FLAG_RDONLY, path);
	if(static_cast<int32>(handle) < 0)
	{
		return -1;
	}

	uint32 result = 0;

	//We don't support loading anything else than all sections
	assert(strcmp(section, "all") == 0);

	auto fileStream(ioman->GetFileStream(handle));
	
	//Load all program sections
	{
		CElfFile executable(*fileStream);
		const auto& header = executable.GetHeader();
		for(unsigned int i = 0; i < header.nProgHeaderCount; i++)
		{
			auto p = executable.GetProgram(i);
			if(p)
			{
				memcpy(m_ram + p->nVAddress, executable.GetContent() + p->nOffset, p->nFileSize);
			}
		}

		result = executable.GetHeader().nEntryPoint;
	}

	//Flush all instruction cache
	OnRequestInstructionCacheFlush();

	ioman->Close(handle);

	return result;
}

void CPS2OS::ApplyPatches()
{
	auto patchesPath = Framework::PathUtils::GetAppResourcesPath() / PATCHESFILENAME;
	
	std::unique_ptr<Framework::Xml::CNode> document;
	try
	{
		Framework::CStdStream patchesStream(Framework::CreateInputStdStream(patchesPath.native()));
		document = std::unique_ptr<Framework::Xml::CNode>(Framework::Xml::CParser::ParseDocument(patchesStream));
		if(!document) return;
	}
	catch(const std::exception& exception)
	{
		printf("Failed to open patch definition file: %s.\r\n", exception.what());
		return;
	}

	auto patchesNode = document->Select("Patches");
	if(patchesNode == NULL)
	{
		return;
	}

	for(Framework::Xml::CFilteringNodeIterator itNode(patchesNode, "Executable"); !itNode.IsEnd(); itNode++)
	{
		auto executableNode = (*itNode);

		const char* sName = executableNode->GetAttribute("Name");
		if(sName == NULL) continue;

		if(!strcmp(sName, GetExecutableName()))
		{
			//Found the right executable
			unsigned int nPatchCount = 0;

			for(Framework::Xml::CFilteringNodeIterator itNode(executableNode, "Patch"); !itNode.IsEnd(); itNode++)
			{
				auto pPatch = (*itNode);
				
				const char* sAddress	= pPatch->GetAttribute("Address");
				const char* sValue		= pPatch->GetAttribute("Value");

				if(sAddress == NULL) continue;
				if(sValue == NULL) continue;

				uint32 nValue = 0, nAddress = 0;
				if(sscanf(sAddress, "%x", &nAddress) == 0) continue;
				if(sscanf(sValue, "%x", &nValue) == 0) continue;

				*(uint32*)&m_ram[nAddress] = nValue;

				nPatchCount++;
			}

			printf("PS2OS: Applied %i patch(es).\r\n", nPatchCount);

			break;
		}
	}
}

void CPS2OS::AssembleCustomSyscallHandler()
{
	CMIPSAssembler Asm((uint32*)&m_bios[0x100]);

	//Epilogue
	Asm.ADDIU(CMIPS::SP, CMIPS::SP, 0xFFF0);
	Asm.SD(CMIPS::RA, 0x0000, CMIPS::SP);
	
	//Load the function address off the table at 0x80010000
	Asm.SLL(CMIPS::T0, CMIPS::V1, 2);
	Asm.LUI(CMIPS::T1, 0x8001);
	Asm.ADDU(CMIPS::T0, CMIPS::T0, CMIPS::T1);
	Asm.LW(CMIPS::T0, 0x0000, CMIPS::T0);
	
	//And the address with 0x1FFFFFFF
	Asm.LUI(CMIPS::T1, 0x1FFF);
	Asm.ORI(CMIPS::T1, CMIPS::T1, 0xFFFF);
	Asm.AND(CMIPS::T0, CMIPS::T0, CMIPS::T1);

	//Jump to the system call address
	Asm.JALR(CMIPS::T0);
	Asm.NOP();

	//Prologue
	Asm.LD(CMIPS::RA, 0x0000, CMIPS::SP);
	Asm.ADDIU(CMIPS::SP, CMIPS::SP, 0x0010);
	Asm.ERET();
}

void CPS2OS::AssembleInterruptHandler()
{
	CMIPSAssembler Asm((uint32*)&m_bios[0x200]);

	const uint32 stackFrameSize = 0x210;

	//Epilogue (allocate stackFrameSize bytes)
	Asm.ADDIU(CMIPS::K0, CMIPS::K0, 0x10000 - stackFrameSize);
	
	//Save context
	for(unsigned int i = 0; i < 32; i++)
	{
		Asm.SQ(i, (i * 0x10), CMIPS::K0);
	}
	
	//Save EPC
	Asm.MFC0(CMIPS::T0, CCOP_SCU::EPC);
	Asm.SW(CMIPS::T0, 0x0200, CMIPS::K0);

	//Set SP
	Asm.ADDU(CMIPS::SP, CMIPS::K0, CMIPS::R0);

	//Get INTC status
	Asm.LUI(CMIPS::T0, 0x1000);
	Asm.ORI(CMIPS::T0, CMIPS::T0, 0xF000);
	Asm.LW(CMIPS::S0, 0x0000, CMIPS::T0);

	//Get INTC mask
	Asm.LUI(CMIPS::T1, 0x1000);
	Asm.ORI(CMIPS::T1, CMIPS::T1, 0xF010);
	Asm.LW(CMIPS::S1, 0x0000, CMIPS::T1);

	//Get cause
	Asm.AND(CMIPS::S0, CMIPS::S0, CMIPS::S1);

	//Clear cause
	//Asm.SW(CMIPS::S0, 0x0000, CMIPS::T0);
	Asm.NOP();

	{
		//Check if INT1 (DMAC)
		Asm.ANDI(CMIPS::T0, CMIPS::S0, 0x0002);
		Asm.BEQ(CMIPS::R0, CMIPS::T0, 0x0005);
		Asm.NOP();

		//Go to DMAC interrupt handler
		Asm.LUI(CMIPS::T0, 0x1FC0);
		Asm.ORI(CMIPS::T0, CMIPS::T0, 0x1000);
		Asm.JALR(CMIPS::T0);
		Asm.NOP();
	}

	{
		//Check if INT2 (Vblank Start)
		Asm.ANDI(CMIPS::T0, CMIPS::S0, 0x0004);
		Asm.BEQ(CMIPS::R0, CMIPS::T0, 0x0006);
		Asm.NOP();

		//Process handlers
		Asm.LUI(CMIPS::T0, 0x1FC0);
		Asm.ORI(CMIPS::T0, CMIPS::T0, 0x2000);
		Asm.ADDIU(CMIPS::A0, CMIPS::R0, 0x0002);
		Asm.JALR(CMIPS::T0);
		Asm.NOP();
	}

	{
		//Check if INT3 (Vblank End)
		Asm.ANDI(CMIPS::T0, CMIPS::S0, 0x0008);
		Asm.BEQ(CMIPS::R0, CMIPS::T0, 0x0006);
		Asm.NOP();

		//Process handlers
		Asm.LUI(CMIPS::T0, 0x1FC0);
		Asm.ORI(CMIPS::T0, CMIPS::T0, 0x2000);
		Asm.ADDIU(CMIPS::A0, CMIPS::R0, 0x0003);
		Asm.JALR(CMIPS::T0);
		Asm.NOP();
	}

	{
		//Check if INT10 (Timer1)
		Asm.ANDI(CMIPS::T0, CMIPS::S0, 0x0400);
		Asm.BEQ(CMIPS::R0, CMIPS::T0, 0x0006);
		Asm.NOP();

		//Process handlers
		Asm.LUI(CMIPS::T0, 0x1FC0);
		Asm.ORI(CMIPS::T0, CMIPS::T0, 0x2000);
		Asm.ADDIU(CMIPS::A0, CMIPS::R0, 0x000A);
		Asm.JALR(CMIPS::T0);
		Asm.NOP();
	}

	{
		//Check if INT11 (Timer2)
		Asm.ANDI(CMIPS::T0, CMIPS::S0, 0x0800);
		Asm.BEQ(CMIPS::R0, CMIPS::T0, 0x0006);
		Asm.NOP();

		//Process handlers
		Asm.LUI(CMIPS::T0, 0x1FC0);
		Asm.ORI(CMIPS::T0, CMIPS::T0, 0x2000);
		Asm.ADDIU(CMIPS::A0, CMIPS::R0, 0x000B);
		Asm.JALR(CMIPS::T0);
		Asm.NOP();
	}

	//Restore EPC
	Asm.LW(CMIPS::T0, 0x0200, CMIPS::K0);
	Asm.MTC0(CMIPS::T0, CCOP_SCU::EPC);

	//Restore Context
	for(unsigned int i = 0; i < 32; i++)
	{
		Asm.LQ(i, (i * 0x10), CMIPS::K0);
	}

	//Prologue
	Asm.ADDIU(CMIPS::K0, CMIPS::K0, stackFrameSize);
	Asm.ERET();
}

void CPS2OS::AssembleDmacHandler()
{
	CMIPSAssembler Asm((uint32*)&m_bios[0x1000]);

	//Prologue
	//S0 -> Channel Counter
	//S1 -> DMA Interrupt Status
	//S2 -> Handler Counter

	Asm.ADDIU(CMIPS::SP, CMIPS::SP, 0xFFE0);
	Asm.SD(CMIPS::RA, 0x0000, CMIPS::SP);
	Asm.SD(CMIPS::S0, 0x0008, CMIPS::SP);
	Asm.SD(CMIPS::S1, 0x0010, CMIPS::SP);
	Asm.SD(CMIPS::S2, 0x0018, CMIPS::SP);

	//Clear INTC cause
	Asm.LUI(CMIPS::T1, 0x1000);
	Asm.ORI(CMIPS::T1, CMIPS::T1, 0xF000);
	Asm.ADDIU(CMIPS::T0, CMIPS::R0, 0x0002);
	Asm.SW(CMIPS::T0, 0x0000, CMIPS::T1);

	//Load the DMA interrupt status
	Asm.LUI(CMIPS::T0, 0x1000);
	Asm.ORI(CMIPS::T0, CMIPS::T0, 0xE010);
	Asm.LW(CMIPS::T0, 0x0000, CMIPS::T0);

	Asm.SRL(CMIPS::T1, CMIPS::T0, 16);
	Asm.AND(CMIPS::S1, CMIPS::T0, CMIPS::T1);

	//Initialize channel counter
	Asm.ADDIU(CMIPS::S0, CMIPS::R0, 0x0009);

	//Check if that specific DMA channel interrupt is the cause
	Asm.ORI(CMIPS::T0, CMIPS::R0, 0x0001);
	Asm.SLLV(CMIPS::T0, CMIPS::T0, CMIPS::S0);
	Asm.AND(CMIPS::T0, CMIPS::T0, CMIPS::S1);
	Asm.BEQ(CMIPS::T0, CMIPS::R0, 0x001A);
	Asm.NOP();

	//Clear interrupt
	Asm.LUI(CMIPS::T1, 0x1000);
	Asm.ORI(CMIPS::T1, CMIPS::T1, 0xE010);
	Asm.SW(CMIPS::T0, 0x0000, CMIPS::T1);

	//Initialize DMAC handler loop
	Asm.ADDU(CMIPS::S2, CMIPS::R0, CMIPS::R0);

	//Get the address to the current DMACHANDLER structure
	Asm.ADDIU(CMIPS::T0, CMIPS::R0, sizeof(DMACHANDLER));
	Asm.MULTU(CMIPS::T0, CMIPS::S2, CMIPS::T0);
	Asm.LUI(CMIPS::T1, 0x8000);
	Asm.ORI(CMIPS::T1, CMIPS::T1, 0xC000);
	Asm.ADDU(CMIPS::T0, CMIPS::T0, CMIPS::T1);

	//Check validity
	Asm.LW(CMIPS::T1, 0x0000, CMIPS::T0);
	Asm.BEQ(CMIPS::T1, CMIPS::R0, 0x000A);
	Asm.NOP();

	//Check if the channel is good one
	Asm.LW(CMIPS::T1, 0x0004, CMIPS::T0);
	Asm.BNE(CMIPS::S0, CMIPS::T1, 0x0007);
	Asm.NOP();

	//Load the necessary stuff
	Asm.LW(CMIPS::T1, 0x0008, CMIPS::T0);
	Asm.ADDU(CMIPS::A0, CMIPS::S0, CMIPS::R0);
	Asm.LW(CMIPS::A1, 0x000C, CMIPS::T0);
	Asm.LW(CMIPS::GP, 0x0010, CMIPS::T0);
	
	//Jump
	Asm.JALR(CMIPS::T1);
	Asm.NOP();

	//Increment handler counter and test
	Asm.ADDIU(CMIPS::S2, CMIPS::S2, 0x0001);
	Asm.ADDIU(CMIPS::T0, CMIPS::R0, MAX_DMACHANDLER - 1);
	Asm.BNE(CMIPS::S2, CMIPS::T0, 0xFFEC);
	Asm.NOP();

	//Decrement channel counter and test
	Asm.ADDIU(CMIPS::S0, CMIPS::S0, 0xFFFF);
	Asm.BGEZ(CMIPS::S0, 0xFFE0);
	Asm.NOP();

	//Epilogue
	Asm.LD(CMIPS::RA, 0x0000, CMIPS::SP);
	Asm.LD(CMIPS::S0, 0x0008, CMIPS::SP);
	Asm.LD(CMIPS::S1, 0x0010, CMIPS::SP);
	Asm.LD(CMIPS::S2, 0x0018, CMIPS::SP);
	Asm.ADDIU(CMIPS::SP, CMIPS::SP, 0x20);
	Asm.JR(CMIPS::RA);
	Asm.NOP();
}

void CPS2OS::AssembleIntcHandler()
{
	CMIPSAssembler assembler(reinterpret_cast<uint32*>(&m_bios[0x2000]));

	CMIPSAssembler::LABEL checkHandlerLabel = assembler.CreateLabel();
	CMIPSAssembler::LABEL moveToNextHandler = assembler.CreateLabel();

	//Prologue
	//S0 -> Handler Counter

	assembler.ADDIU(CMIPS::SP, CMIPS::SP, 0xFFE0);
	assembler.SD(CMIPS::RA, 0x0000, CMIPS::SP);
	assembler.SD(CMIPS::S0, 0x0008, CMIPS::SP);
	assembler.SD(CMIPS::S1, 0x0010, CMIPS::SP);

	//Clear INTC cause
	assembler.LUI(CMIPS::T1, 0x1000);
	assembler.ORI(CMIPS::T1, CMIPS::T1, 0xF000);
	assembler.ADDIU(CMIPS::T0, CMIPS::R0, 0x0001);
	assembler.SLLV(CMIPS::T0, CMIPS::T0, CMIPS::A0);
	assembler.SW(CMIPS::T0, 0x0000, CMIPS::T1);

	//Initialize INTC handler loop
	assembler.ADDU(CMIPS::S0, CMIPS::R0, CMIPS::R0);
	assembler.ADDU(CMIPS::S1, CMIPS::A0, CMIPS::R0);

	assembler.MarkLabel(checkHandlerLabel);

	//Get the address to the current INTCHANDLER structure
	assembler.ADDIU(CMIPS::T0, CMIPS::R0, sizeof(INTCHANDLER));
	assembler.MULTU(CMIPS::T0, CMIPS::S0, CMIPS::T0);
	assembler.LUI(CMIPS::T1, 0x8000);
	assembler.ORI(CMIPS::T1, CMIPS::T1, 0xA000);
	assembler.ADDU(CMIPS::T0, CMIPS::T0, CMIPS::T1);

	//Check validity
	assembler.LW(CMIPS::T1, 0x0000, CMIPS::T0);
	assembler.BEQ(CMIPS::T1, CMIPS::R0, moveToNextHandler);
	assembler.NOP();

	//Check if the cause is good one
	assembler.LW(CMIPS::T1, 0x0004, CMIPS::T0);
	assembler.BNE(CMIPS::S1, CMIPS::T1, moveToNextHandler);
	assembler.NOP();

	//Load the necessary stuff
	assembler.LW(CMIPS::T1, 0x0008, CMIPS::T0);
	assembler.ADDU(CMIPS::A0, CMIPS::S1, CMIPS::R0);
	assembler.LW(CMIPS::A1, 0x000C, CMIPS::T0);
	assembler.LW(CMIPS::GP, 0x0010, CMIPS::T0);
	
	//Jump
	assembler.JALR(CMIPS::T1);
	assembler.NOP();

	assembler.MarkLabel(moveToNextHandler);

	//Increment handler counter and test
	assembler.ADDIU(CMIPS::S0, CMIPS::S0, 0x0001);
	assembler.ADDIU(CMIPS::T0, CMIPS::R0, MAX_INTCHANDLER - 1);
	assembler.BNE(CMIPS::S0, CMIPS::T0, checkHandlerLabel);
	assembler.NOP();

	//Epilogue
	assembler.LD(CMIPS::RA, 0x0000, CMIPS::SP);
	assembler.LD(CMIPS::S0, 0x0008, CMIPS::SP);
	assembler.LD(CMIPS::S1, 0x0010, CMIPS::SP);
	assembler.ADDIU(CMIPS::SP, CMIPS::SP, 0x20);
	assembler.JR(CMIPS::RA);
	assembler.NOP();
}

void CPS2OS::AssembleThreadEpilog()
{
	CMIPSAssembler Asm((uint32*)&m_bios[0x3000]);
	
	Asm.ADDIU(CMIPS::V1, CMIPS::R0, 0x23);
	Asm.SYSCALL();
}

void CPS2OS::AssembleWaitThreadProc()
{
	CMIPSAssembler Asm((uint32*)&m_bios[BIOS_ADDRESS_WAITTHREADPROC - BIOS_ADDRESS_BASE]);

	Asm.ADDIU(CMIPS::V1, CMIPS::R0, 0x666);
	Asm.SYSCALL();

	Asm.BEQ(CMIPS::R0, CMIPS::R0, 0xFFFD);
	Asm.NOP();
}

uint32* CPS2OS::GetCustomSyscallTable()
{
	return (uint32*)&m_ram[0x00010000];
}

uint32 CPS2OS::GetCurrentThreadId() const
{
	return *(uint32*)&m_ram[0x00000000];
}

void CPS2OS::SetCurrentThreadId(uint32 nThread)
{
	*(uint32*)&m_ram[0x00000000] = nThread;
}

uint32 CPS2OS::GetNextAvailableThreadId()
{
	for(uint32 i = 0; i < MAX_THREAD; i++)
	{
		THREAD* pThread = GetThread(i);
		if(pThread->nValid != 1)
		{
			return i;
		}
	}

	return 0xFFFFFFFF;
}

CPS2OS::THREAD* CPS2OS::GetThread(uint32 nID) const
{
	return &((THREAD*)&m_ram[0x00011000])[nID];
}

void CPS2OS::ThreadShakeAndBake()
{
	//Don't play with fire (don't switch if we're in exception mode)
	if(m_ee.m_State.nCOP0[CCOP_SCU::STATUS] & CMIPS::STATUS_EXL)
	{
		return;
	}

	//Don't switch if interrupts are disabled
	if(!(m_ee.m_State.nCOP0[CCOP_SCU::STATUS] & CMIPS::STATUS_INT))
	{
		return;
	}

	//First of all, revoke the current's thread right to execute itself
	{
		unsigned int nId = GetCurrentThreadId();
		if(nId != 0)
		{
			THREAD* pThread = GetThread(nId);
			pThread->nQuota--;
		}
	}

	//Check if all quotas expired
	if(ThreadHasAllQuotasExpired())
	{
		CRoundRibbon::ITERATOR itThread(m_pThreadSchedule);

		//If so, regive a quota to everyone
		for(itThread = m_pThreadSchedule->Begin(); !itThread.IsEnd(); itThread++)
		{
			unsigned int nId = itThread.GetValue();
			THREAD* pThread = GetThread(nId);

			pThread->nQuota = THREAD_INIT_QUOTA;
		}
	}

	//Select thread to execute
	{
		unsigned int nId = 0;
		THREAD* pThread = NULL;
		CRoundRibbon::ITERATOR itThread(m_pThreadSchedule);

		//Next, find the next suitable thread to execute
		for(itThread = m_pThreadSchedule->Begin(); !itThread.IsEnd(); itThread++)
		{
			nId = itThread.GetValue();
			pThread = GetThread(nId);

			if(pThread->nStatus != THREAD_RUNNING) continue;
			//if(pThread->nQuota == 0) continue;
			break;
		}

		if(itThread.IsEnd())
		{
			//Deadlock or something here
			//printf("%s: Warning, no thread to execute.\r\n", LOG_NAME);
			nId = 0;
		}
		else
		{
			//Remove and readd the thread into the queue
			m_pThreadSchedule->Remove(pThread->nScheduleID);
			pThread->nScheduleID = m_pThreadSchedule->Insert(nId, pThread->nPriority);
		}

		ThreadSwitchContext(nId);
	}
}

bool CPS2OS::ThreadHasAllQuotasExpired()
{
	CRoundRibbon::ITERATOR itThread(m_pThreadSchedule);

	for(itThread = m_pThreadSchedule->Begin(); !itThread.IsEnd(); itThread++)
	{
		unsigned int nId = itThread.GetValue();
		THREAD* pThread = GetThread(nId);

		if(pThread->nStatus != THREAD_RUNNING) continue;
		if(pThread->nQuota == 0) continue;

		return false;
	}

	return true;
}

void CPS2OS::ThreadSwitchContext(unsigned int nID)
{
	if(nID == GetCurrentThreadId()) return;

	//Save the context of the current thread
	{
		THREAD* pThread = GetThread(GetCurrentThreadId());
		THREADCONTEXT* pContext = reinterpret_cast<THREADCONTEXT*>(&m_ram[pThread->nContextPtr]);

		//Save the context
		for(uint32 i = 0; i < 0x20; i++)
		{
			if(i == CMIPS::R0) continue;
			if(i == CMIPS::K0) continue;
			if(i == CMIPS::K1) continue;
			pContext->nGPR[i] = m_ee.m_State.nGPR[i];
		}

		pThread->nEPC = m_ee.m_State.nPC;
	}

	SetCurrentThreadId(nID);

	//Load the new context
	{
		THREAD* pThread = GetThread(GetCurrentThreadId());
		THREADCONTEXT* pContext = reinterpret_cast<THREADCONTEXT*>(&m_ram[pThread->nContextPtr]);

		m_ee.m_State.nPC = pThread->nEPC;

		for(uint32 i = 0; i < 0x20; i++)
		{
			if(i == CMIPS::R0) continue;
			if(i == CMIPS::K0) continue;
			if(i == CMIPS::K1) continue;
			m_ee.m_State.nGPR[i] = pContext->nGPR[i];
		}
	}

	CLog::GetInstance().Print(LOG_NAME, "New thread elected (id = %i).\r\n", nID);
}

void CPS2OS::CreateWaitThread()
{
	THREAD* pThread = GetThread(0);
	pThread->nValid		= 1;
	pThread->nEPC		= BIOS_ADDRESS_WAITTHREADPROC;
	pThread->nStatus	= THREAD_ZOMBIE;
}

uint32 CPS2OS::GetNextAvailableSemaphoreId()
{
	for(uint32 i = 1; i < MAX_SEMAPHORE; i++)
	{
		SEMAPHORE* pSemaphore = GetSemaphore(i);
		if(pSemaphore->nValid != 1)
		{
			return i;
		}
	}

	return 0xFFFFFFFF;
}

CPS2OS::SEMAPHORE* CPS2OS::GetSemaphore(uint32 nID)
{
	if(nID == 0)
	{
		return NULL;
	}
	nID--;
	return &((SEMAPHORE*)&m_ram[0x0000E000])[nID];
}

uint32 CPS2OS::GetNextAvailableDmacHandlerId()
{
	for(uint32 i = 1; i < MAX_DMACHANDLER; i++)
	{
		DMACHANDLER* pHandler = GetDmacHandler(i);
		if(pHandler->nValid != 1)
		{
			return i;
		}
	}

	return 0xFFFFFFFF;
}

CPS2OS::DMACHANDLER* CPS2OS::GetDmacHandler(uint32 nID)
{
	nID--;
	return &((DMACHANDLER*)&m_ram[0x0000C000])[nID];
}

uint32 CPS2OS::GetNextAvailableIntcHandlerId()
{
	for(uint32 i = 1; i < MAX_INTCHANDLER; i++)
	{
		INTCHANDLER* pHandler = GetIntcHandler(i);
		if(pHandler->nValid != 1)
		{
			return i;
		}
	}

	return 0xFFFFFFFF;
}

CPS2OS::INTCHANDLER* CPS2OS::GetIntcHandler(uint32 nID)
{
	nID--;
	return &((INTCHANDLER*)&m_ram[0x0000A000])[nID];
}

uint32 CPS2OS::GetNextAvailableDeci2HandlerId()
{
	for(uint32 i = 1; i < MAX_DECI2HANDLER; i++)
	{
		DECI2HANDLER* pHandler = GetDeci2Handler(i);
		if(pHandler->nValid != 1)
		{
			return i;
		}
	}

	return 0xFFFFFFFF;
}

CPS2OS::DECI2HANDLER* CPS2OS::GetDeci2Handler(uint32 nID)
{
	nID--;
	return &((DECI2HANDLER*)&m_ram[0x00008000])[nID];
}

void CPS2OS::ExceptionHandler()
{
	m_semaWaitCount = 0;
	ThreadShakeAndBake();
	m_ee.GenerateInterrupt(0x1FC00200);
}

uint32 CPS2OS::TranslateAddress(CMIPS* pCtx, uint32 nVAddrLO)
{
	if(nVAddrLO >= 0x70000000 && nVAddrLO <= 0x70003FFF)
	{
		return (nVAddrLO - 0x6E000000);
	}
	if(nVAddrLO >= 0x30100000 && nVAddrLO <= 0x31FFFFFF)
	{
		return (nVAddrLO - 0x30000000);
	}
	return nVAddrLO & 0x1FFFFFFF;
}

//////////////////////////////////////////////////
//System Calls
//////////////////////////////////////////////////

void CPS2OS::sc_Unhandled()
{
	printf("PS2OS: Unknown system call (0x%X) called from 0x%0.8X.\r\n", m_ee.m_State.nGPR[3].nV[0], m_ee.m_State.nPC);
}

//02
void CPS2OS::sc_GsSetCrt()
{
	bool nIsInterlaced			= (m_ee.m_State.nGPR[SC_PARAM0].nV[0] != 0);
	unsigned int nMode			= m_ee.m_State.nGPR[SC_PARAM1].nV[0];
	bool nIsFrameMode			= (m_ee.m_State.nGPR[SC_PARAM2].nV[0] != 0);

	if(m_gs != NULL)
	{
		m_gs->SetCrt(nIsInterlaced, nMode, nIsFrameMode);
	}
}

//06
void CPS2OS::sc_LoadExecPS2()
{
	uint32 fileNamePtr	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 argCount		= m_ee.m_State.nGPR[SC_PARAM1].nV[0];
	uint32 argValuesPtr	= m_ee.m_State.nGPR[SC_PARAM2].nV[0];

	ArgumentList arguments;
	for(uint32 i = 0; i < argCount; i++)
	{
		uint32 argValuePtr = *reinterpret_cast<uint32*>(m_ram + argValuesPtr + i * 4);
		arguments.push_back(reinterpret_cast<const char*>(m_ram + argValuePtr));
	}

	std::string fileName = reinterpret_cast<const char*>(m_ram + fileNamePtr);
	OnRequestLoadExecutable(fileName.c_str(), arguments);
}

//10
void CPS2OS::sc_AddIntcHandler()
{
	uint32 nCause	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nAddress	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];
	uint32 nNext	= m_ee.m_State.nGPR[SC_PARAM2].nV[0];
	uint32 nArg		= m_ee.m_State.nGPR[SC_PARAM3].nV[0];

	/*
	if(nNext != 0)
	{
		assert(0);
	}
	*/

	uint32 nID = GetNextAvailableIntcHandlerId();
	if(nID == 0xFFFFFFFF)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	INTCHANDLER* pHandler = GetIntcHandler(nID);
	pHandler->nValid	= 1;
	pHandler->nAddress	= nAddress;
	pHandler->nCause	= nCause;
	pHandler->nArg		= nArg;
	pHandler->nGP		= m_ee.m_State.nGPR[CMIPS::GP].nV[0];

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//11
void CPS2OS::sc_RemoveIntcHandler()
{
	uint32 nCause	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nID		= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	INTCHANDLER* pHandler = GetIntcHandler(nID);
	if(pHandler->nValid != 1)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	pHandler->nValid = 0;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//12
void CPS2OS::sc_AddDmacHandler()
{
	uint32 nChannel	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nAddress	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];
	uint32 nNext	= m_ee.m_State.nGPR[SC_PARAM2].nV[0];
	uint32 nArg		= m_ee.m_State.nGPR[SC_PARAM3].nV[0];

	//The Next parameter indicates at which moment we'd want our DMAC handler to be called.
	//-1 -> At the end
	//0  -> At the start
	//n  -> After handler 'n'

	if(nNext != 0)
	{
		assert(0);
	}

	uint32 nID = GetNextAvailableDmacHandlerId();
	if(nID == 0xFFFFFFFF)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	DMACHANDLER* pHandler = GetDmacHandler(nID);
	pHandler->nValid	= 1;
	pHandler->nAddress	= nAddress;
	pHandler->nChannel	= nChannel;
	pHandler->nArg		= nArg;
	pHandler->nGP		= m_ee.m_State.nGPR[CMIPS::GP].nV[0];

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//13
void CPS2OS::sc_RemoveDmacHandler()
{
	uint32 nChannel	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nID		= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	DMACHANDLER* pHandler = GetDmacHandler(nID);
	pHandler->nValid = 0x00;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//14
void CPS2OS::sc_EnableIntc()
{
	uint32 nCause = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nMask = 1 << nCause;

	if(!(m_ee.m_pMemoryMap->GetWord(CINTC::INTC_MASK) & nMask))
	{
		m_ee.m_pMemoryMap->SetWord(CINTC::INTC_MASK, nMask);
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 1;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//15
void CPS2OS::sc_DisableIntc()
{
	uint32 nCause = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nMask = 1 << nCause;
	if(m_ee.m_pMemoryMap->GetWord(CINTC::INTC_MASK) & nMask)
	{
		m_ee.m_pMemoryMap->SetWord(CINTC::INTC_MASK, nMask);
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 1;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//16
void CPS2OS::sc_EnableDmac()
{
	uint32 nChannel = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nRegister = 0x10000 << nChannel;

	if(!(m_ee.m_pMemoryMap->GetWord(CDMAC::D_STAT) & nRegister))
	{
		m_ee.m_pMemoryMap->SetWord(CDMAC::D_STAT, nRegister);
	}

	//Enable INT1
	if(!(m_ee.m_pMemoryMap->GetWord(CINTC::INTC_MASK) & 0x02))
	{
		m_ee.m_pMemoryMap->SetWord(CINTC::INTC_MASK, 0x02);
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 1;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//17
void CPS2OS::sc_DisableDmac()
{
	uint32 nChannel = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nRegister = 0x10000 << nChannel;

	if(m_ee.m_pMemoryMap->GetWord(CDMAC::D_STAT) & nRegister)
	{
		m_ee.m_pMemoryMap->SetWord(CDMAC::D_STAT, nRegister);
		m_ee.m_State.nGPR[SC_RETURN].nD0 = 1;
	}
	else
	{
		m_ee.m_State.nGPR[SC_RETURN].nD0 = 0;
	}
}

//20
void CPS2OS::sc_CreateThread()
{
	THREADPARAM* pThreadParam = (THREADPARAM*)&m_ram[m_ee.m_State.nGPR[SC_PARAM0].nV[0]];

	uint32 nID = GetNextAvailableThreadId();
	if(nID == 0xFFFFFFFF)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
		return;
	}

	THREAD* pThread = GetThread(GetCurrentThreadId());
	uint32 nHeapBase = pThread->nHeapBase;

	assert(pThreadParam->nPriority < 128);

	pThread = GetThread(nID);
	pThread->nValid			= 1;
	pThread->nStatus		= THREAD_ZOMBIE;
	pThread->nStackBase		= pThreadParam->nStackBase;
	pThread->nEPC			= pThreadParam->nThreadProc;
	pThread->nPriority		= pThreadParam->nPriority;
	pThread->nHeapBase		= nHeapBase;
	pThread->nWakeUpCount	= 0;
	pThread->nQuota			= THREAD_INIT_QUOTA;
	pThread->nScheduleID	= m_pThreadSchedule->Insert(nID, pThreadParam->nPriority);
	pThread->nStackSize		= pThreadParam->nStackSize;

	uint32 nStackAddr = pThreadParam->nStackBase + pThreadParam->nStackSize - STACKRES;
	pThread->nContextPtr	= nStackAddr;

	assert(sizeof(THREADCONTEXT) == STACKRES);

	THREADCONTEXT* pContext = reinterpret_cast<THREADCONTEXT*>(&m_ram[pThread->nContextPtr]);
	memset(pContext, 0, sizeof(THREADCONTEXT));

	pContext->nGPR[CMIPS::SP].nV0 = nStackAddr;
	pContext->nGPR[CMIPS::FP].nV0 = nStackAddr;
	pContext->nGPR[CMIPS::GP].nV0 = pThreadParam->nGP;
	pContext->nGPR[CMIPS::RA].nV0 = 0x1FC03000;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//21
void CPS2OS::sc_DeleteThread()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	m_pThreadSchedule->Remove(pThread->nScheduleID);

	pThread->nValid = 0;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//22
void CPS2OS::sc_StartThread()
{
	uint32 nID	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nArg	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	assert(pThread->nStatus == THREAD_ZOMBIE);
	pThread->nStatus = THREAD_RUNNING;

	THREADCONTEXT* pContext = reinterpret_cast<THREADCONTEXT*>(&m_ram[pThread->nContextPtr]);
	pContext->nGPR[CMIPS::A0].nV0 = nArg;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//23
void CPS2OS::sc_ExitThread()
{
	THREAD* pThread = GetThread(GetCurrentThreadId());
	pThread->nStatus = THREAD_ZOMBIE;

	ThreadShakeAndBake();
}

//25
void CPS2OS::sc_TerminateThread()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	pThread->nStatus = THREAD_ZOMBIE;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//29
//2A
void CPS2OS::sc_ChangeThreadPriority()
{
	bool nInt		= m_ee.m_State.nGPR[3].nV[0] == 0x2A;
	uint32 nID		= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nPrio	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	uint32 nPrevPrio = pThread->nPriority;
	pThread->nPriority = nPrio;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nPrevPrio;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;

	//Reschedule?
	m_pThreadSchedule->Remove(pThread->nScheduleID);
	pThread->nScheduleID = m_pThreadSchedule->Insert(nID, pThread->nPriority);

	if(!nInt)
	{
		ThreadShakeAndBake();
	}
}

//2B
void CPS2OS::sc_RotateThreadReadyQueue()
{
	CRoundRibbon::ITERATOR itThread(m_pThreadSchedule);

	uint32 nPrio = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	//TODO: Rescheduling isn't always necessary and will cause the current thread's priority queue to be
	//rotated too since each time a thread is picked to be executed it's placed at the end of the queue...

	//Find first of this priority and reinsert if it's the same as the current thread
	//If it's not the same, the schedule will be rotated when another thread is choosen
	for(itThread = m_pThreadSchedule->Begin(); !itThread.IsEnd(); itThread++)
	{
		if(itThread.GetWeight() == nPrio)
		{
			uint32 nID = itThread.GetValue();
			if(nID == GetCurrentThreadId())
			{
				throw std::runtime_error("Need to reverify that.");
				THREAD* thread(GetThread(nID));
				m_pThreadSchedule->Remove(itThread.GetIndex());
				thread->nScheduleID = m_pThreadSchedule->Insert(nID, nPrio);
			}
			break;
		}
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nPrio;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;

	if(!itThread.IsEnd())
	{
		//Change has been made
		ThreadShakeAndBake();
	}
}

//2F
void CPS2OS::sc_GetThreadId()
{
	m_ee.m_State.nGPR[SC_RETURN].nV[0] = GetCurrentThreadId();
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//30
void CPS2OS::sc_ReferThreadStatus()
{
	uint32 nID			= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nStatusPtr	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	nStatusPtr &= (PS2::EE_RAM_SIZE - 1);

	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	//THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04, THS_SUSPEND = 0x08, THS_DORMANT = 0x10
	uint32 nRet = 0;
	switch(pThread->nStatus)
	{
	case THREAD_RUNNING:
		nRet = 0x01;
		break;
	case THREAD_WAITING:
	case THREAD_SLEEPING:
		nRet = 0x04;
		break;
	case THREAD_SUSPENDED:
		nRet = 0x08;
		break;
	case THREAD_SUSPENDED_WAITING:
	case THREAD_SUSPENDED_SLEEPING:
		nRet = 0x0C;
		break;
	case THREAD_ZOMBIE:
		nRet = 0x10;
		break;
	}

	if(nStatusPtr != 0)
	{
		THREADPARAM* pThreadParam = reinterpret_cast<THREADPARAM*>(&m_ram[nStatusPtr]);

		pThreadParam->nStatus			= nRet;
		pThreadParam->nPriority			= pThread->nPriority;
		pThreadParam->nCurrentPriority	= pThread->nPriority;
		pThreadParam->nStackBase		= pThread->nStackBase;
		pThreadParam->nStackSize		= pThread->nStackSize;
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nRet;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//32
void CPS2OS::sc_SleepThread()
{
	THREAD* pThread = GetThread(GetCurrentThreadId());
	if(pThread->nWakeUpCount == 0)
	{
		assert(pThread->nStatus == THREAD_RUNNING);
		pThread->nStatus = THREAD_SLEEPING;
		ThreadShakeAndBake();
		return;
	}

	pThread->nWakeUpCount--;
}

//33
//34
void CPS2OS::sc_WakeupThread()
{
	uint32 nID		= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	bool nInt		= m_ee.m_State.nGPR[3].nV[0] == 0x34;

	THREAD* pThread = GetThread(nID);

	if(
		(pThread->nStatus == THREAD_SLEEPING) || 
		(pThread->nStatus == THREAD_SUSPENDED_SLEEPING))
	{
		switch(pThread->nStatus)
		{
		case THREAD_SLEEPING:
			pThread->nStatus = THREAD_RUNNING;
			break;
		case THREAD_SUSPENDED_SLEEPING:
			pThread->nStatus = THREAD_SUSPENDED;
			break;
		default:
			assert(0);
			break;
		}
		ThreadShakeAndBake();
	}
	else
	{
		pThread->nWakeUpCount++;
	}
}

//37
void CPS2OS::sc_SuspendThread()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	
	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		return;
	}

	switch(pThread->nStatus)
	{
	case THREAD_RUNNING:
		pThread->nStatus = THREAD_SUSPENDED;
		break;
	case THREAD_WAITING:
		pThread->nStatus = THREAD_SUSPENDED_WAITING;
		break;
	case THREAD_SLEEPING:
		pThread->nStatus = THREAD_SUSPENDED_SLEEPING;
		break;
	default:
		assert(0);
		break;
	}

	ThreadShakeAndBake();
}

//39
void CPS2OS::sc_ResumeThread()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	THREAD* pThread = GetThread(nID);
	if(!pThread->nValid)
	{
		return;
	}

	switch(pThread->nStatus)
	{
	case THREAD_SUSPENDED:
		pThread->nStatus = THREAD_RUNNING;
		break;
	case THREAD_SUSPENDED_WAITING:
		pThread->nStatus = THREAD_WAITING;
		break;
	case THREAD_SUSPENDED_SLEEPING:
		pThread->nStatus = THREAD_SLEEPING;
		break;
	default:
		assert(0);
		break;
	}

	ThreadShakeAndBake();
}

//3C
void CPS2OS::sc_SetupThread()
{
	uint32 nStackBase = m_ee.m_State.nGPR[SC_PARAM1].nV[0];
	uint32 nStackSize = m_ee.m_State.nGPR[SC_PARAM2].nV[0];

	uint32 nStackAddr = 0;
	if(nStackBase == 0xFFFFFFFF)
	{
		nStackAddr = 0x02000000;
	}
	else
	{
		nStackAddr = nStackBase + nStackSize;
	}

	uint32 argsBase = m_ee.m_State.nGPR[SC_PARAM3].nV[0];
	//Copy arguments
	{
		ArgumentList completeArgList;
		completeArgList.push_back(m_executableName);
		completeArgList.insert(completeArgList.end(), m_currentArguments.begin(), m_currentArguments.end());

		uint32 argsCount = static_cast<uint32>(completeArgList.size());

		*reinterpret_cast<uint32*>(m_ram + argsBase) = argsCount;
		uint32 argsPtrs = argsBase + 4;
		uint32 argsPayload = argsPtrs + (argsCount * 4);
		for(uint32 i = 0; i < argsCount; i++)
		{
			const auto& currentArg = completeArgList[i];
			*reinterpret_cast<uint32*>(m_ram + argsPtrs + (i * 4)) = argsPayload;
			uint32 argSize = static_cast<uint32>(currentArg.size()) + 1;
			memcpy(m_ram + argsPayload, currentArg.c_str(), argSize);
			argsPayload += argSize;
		}
	}

	//Set up the main thread
	THREAD* pThread = GetThread(1);
	pThread->nValid			= 0x01;
	pThread->nStatus		= THREAD_RUNNING;
	pThread->nStackBase		= nStackAddr - nStackSize;
	pThread->nPriority		= 0;
	pThread->nQuota			= THREAD_INIT_QUOTA;
	pThread->nScheduleID	= m_pThreadSchedule->Insert(1, pThread->nPriority);

	nStackAddr -= STACKRES;
	pThread->nContextPtr	= nStackAddr;

	SetCurrentThreadId(1);

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nStackAddr;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//3D
void CPS2OS::sc_SetupHeap()
{
	THREAD* pThread = GetThread(GetCurrentThreadId());

	uint32 nHeapBase = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nHeapSize = m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	if(nHeapSize == 0xFFFFFFFF)
	{
		pThread->nHeapBase = pThread->nStackBase;
	}
	else
	{
		pThread->nHeapBase = nHeapBase + nHeapSize;
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = pThread->nHeapBase;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//3E
void CPS2OS::sc_EndOfHeap()
{
	THREAD* pThread = GetThread(GetCurrentThreadId());

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = pThread->nHeapBase;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//40
void CPS2OS::sc_CreateSema()
{
	SEMAPHOREPARAM* pSemaParam = reinterpret_cast<SEMAPHOREPARAM*>(m_ram + m_ee.m_State.nGPR[SC_PARAM0].nV[0]);

	uint32 nID = GetNextAvailableSemaphoreId();
	if(nID == 0xFFFFFFFF)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	SEMAPHORE* pSema = GetSemaphore(nID);
	pSema->nValid		= 1;
	pSema->nCount		= pSemaParam->nInitCount;
	pSema->nMaxCount	= pSemaParam->nMaxCount;
	pSema->nWaitCount	= 0;

	assert(pSema->nCount <= pSema->nMaxCount);

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//41
void CPS2OS::sc_DeleteSema()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	SEMAPHORE* pSema = GetSemaphore(nID);
	if(!pSema->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	//Check if any threads are waiting for this?
	if(pSema->nWaitCount != 0)
	{
		assert(0);
	}

	pSema->nValid = 0;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//42
//43
void CPS2OS::sc_SignalSema()
{
	bool nInt	= m_ee.m_State.nGPR[3].nV[0] == 0x43;
	uint32 nID	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	SEMAPHORE* pSema = GetSemaphore(nID);
	if(!pSema || !pSema->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}
	
	if(pSema->nWaitCount != 0)
	{
		//Unsleep all threads if they were waiting
		for(uint32 i = 0; i < MAX_THREAD; i++)
		{
			THREAD* pThread = GetThread(i);
			if(!pThread->nValid) continue;
			if((pThread->nStatus != THREAD_WAITING) && (pThread->nStatus != THREAD_SUSPENDED_WAITING)) continue;
			if(pThread->nSemaWait != nID) continue;

			switch(pThread->nStatus)
			{
			case THREAD_WAITING:
				pThread->nStatus = THREAD_RUNNING;
				break;
			case THREAD_SUSPENDED_WAITING:
				pThread->nStatus = THREAD_SUSPENDED;
				break;
			default:
				assert(0);
				break;
			}
			pThread->nQuota = THREAD_INIT_QUOTA;
			pSema->nWaitCount--;

			if(pSema->nWaitCount == 0)
			{
				break;
			}
		}

		m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;

		if(!nInt)
		{
			ThreadShakeAndBake();
		}
	}
	else
	{
		pSema->nCount++;
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//44
void CPS2OS::sc_WaitSema()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	SEMAPHORE* pSema = GetSemaphore(nID);
	if(!pSema || !pSema->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	if((m_semaWaitId == nID) && (m_semaWaitCaller == m_ee.m_State.nGPR[CMIPS::RA].nV0))
	{
		m_semaWaitCount++;
		if(m_semaWaitCount > 100)
		{
			m_semaWaitThreadId = GetCurrentThreadId();
		}
	}
	else
	{
		m_semaWaitId = nID;
		m_semaWaitCaller = m_ee.m_State.nGPR[CMIPS::RA].nV0;
		m_semaWaitCount = 0;
	}

	if(pSema->nCount == 0)
	{
		//Put this thread in sleep mode and reschedule...
		pSema->nWaitCount++;

		THREAD* pThread = GetThread(GetCurrentThreadId());
		assert(pThread->nStatus == THREAD_RUNNING);
		pThread->nStatus	= THREAD_WAITING;
		pThread->nSemaWait	= nID;

		ThreadShakeAndBake();

		return;
	}

	if(pSema->nCount != 0)
	{
		pSema->nCount--;
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//45
void CPS2OS::sc_PollSema()
{
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	SEMAPHORE* pSema = GetSemaphore(nID);
	if(!pSema->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	if(pSema->nCount == 0)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	pSema->nCount--;
	
	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//47
//48
void CPS2OS::sc_ReferSemaStatus()
{
	bool isInt = m_ee.m_State.nGPR[3].nV[0] != 0x47;
	uint32 nID = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	SEMAPHOREPARAM* pSemaParam = (SEMAPHOREPARAM*)(m_ram + (m_ee.m_State.nGPR[SC_PARAM1].nV[0] & 0x1FFFFFFF));

	SEMAPHORE* pSema = GetSemaphore(nID);
	if(!pSema->nValid)
	{
		m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
		m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
		return;
	}

	pSemaParam->nCount			= pSema->nCount;
	pSemaParam->nMaxCount		= pSema->nMaxCount;
	pSemaParam->nWaitThreads	= pSema->nWaitCount;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//64
void CPS2OS::sc_FlushCache()
{
	uint32 operationType = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	if(operationType == 2)
	{
		//Flush instruction cache
		OnRequestInstructionCacheFlush();
	}
}

//70
void CPS2OS::sc_GsGetIMR()
{
	uint32 result = 0;

	if(m_gs != NULL)
	{
		result = m_gs->ReadPrivRegister(CGSHandler::GS_IMR);
	}

	m_ee.m_State.nGPR[SC_RETURN].nD0 = static_cast<int32>(result);
}

//71
void CPS2OS::sc_GsPutIMR()
{
	uint32 nIMR = m_ee.m_State.nGPR[SC_PARAM0].nV[0];

	if(m_gs != NULL)
	{
		m_gs->WritePrivRegister(CGSHandler::GS_IMR, nIMR);
	}
}

//73
void CPS2OS::sc_SetVSyncFlag()
{
	uint32 nPtr1	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nPtr2	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	*(uint32*)&m_ram[nPtr1] = 0x01;

	if(m_gs != NULL)
	{
		//*(uint32*)&m_ram[nPtr2] = 0x2000;
		*(uint32*)&m_ram[nPtr2] = m_gs->ReadPrivRegister(CGSHandler::GS_CSR) & 0x2000;
	}
	else
	{
		//Humm...
		*(uint32*)&m_ram[nPtr2] = 0;
	}

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//74
void CPS2OS::sc_SetSyscall()
{
	uint8 nNumber	= static_cast<uint8>(m_ee.m_State.nGPR[SC_PARAM0].nV[0] & 0xFF);
	uint32 nAddress	= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	GetCustomSyscallTable()[nNumber]	= nAddress;

	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//76
void CPS2OS::sc_SifDmaStat()
{
	m_ee.m_State.nGPR[SC_RETURN].nV[0] = 0xFFFFFFFF;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0xFFFFFFFF;
}

//77
void CPS2OS::sc_SifSetDma()
{
	struct DMAREG
	{
		uint32 nSrcAddr;
		uint32 nDstAddr;
		uint32 nSize;
		uint32 nFlags;
	};

	uint32 xferAddress = m_ee.m_State.nGPR[SC_PARAM0].nV[0] & (PS2::EE_RAM_SIZE - 1);
	DMAREG* pXfer = reinterpret_cast<DMAREG*>(m_ram + xferAddress);
	uint32 nCount = m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	//Returns count
	//DMA might call an interrupt handler
	m_ee.m_State.nGPR[SC_RETURN].nD0 = static_cast<int32>(nCount);

	for(unsigned int i = 0; i < nCount; i++)
	{
		uint32 nSize = (pXfer[i].nSize + 0x0F) / 0x10;

		m_ee.m_pMemoryMap->SetWord(CDMAC::D6_MADR,	pXfer[i].nSrcAddr);
		m_ee.m_pMemoryMap->SetWord(CDMAC::D6_TADR,	pXfer[i].nDstAddr);
		m_ee.m_pMemoryMap->SetWord(CDMAC::D6_QWC,	nSize);
		m_ee.m_pMemoryMap->SetWord(CDMAC::D6_CHCR,	0x00000100);
	}
}

//78
void CPS2OS::sc_SifSetDChain()
{
	//Humm, set the SIF0 DMA channel in destination chain mode?
}

//79
void CPS2OS::sc_SifSetReg()
{
	uint32 nRegister	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nValue		= m_ee.m_State.nGPR[SC_PARAM1].nV[0];

	m_sif.SetRegister(nRegister, nValue);

	m_ee.m_State.nGPR[SC_RETURN].nD0 = 0;
}

//7A
void CPS2OS::sc_SifGetReg()
{
	uint32 nRegister = m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	m_ee.m_State.nGPR[SC_RETURN].nD0 = static_cast<int32>(m_sif.GetRegister(nRegister));
}

//7C
void CPS2OS::sc_Deci2Call()
{
	uint32 nFunction	= m_ee.m_State.nGPR[SC_PARAM0].nV[0];
	uint32 nParam		= m_ee.m_State.nGPR[SC_PARAM1].nV[0];
	
	switch(nFunction)
	{
	case 0x01:
		//Deci2Open
		{
			uint32 nID = GetNextAvailableDeci2HandlerId();

			DECI2HANDLER* pHandler = GetDeci2Handler(nID);
			pHandler->nValid		= 1;
			pHandler->nDevice		= *(uint32*)&m_ram[nParam + 0x00];
			pHandler->nBufferAddr	= *(uint32*)&m_ram[nParam + 0x04];

			m_ee.m_State.nGPR[SC_RETURN].nV[0] = nID;
			m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
		}
		break;
	case 0x03:
		//Deci2Send
		{
			uint32 nID = *reinterpret_cast<uint32*>(&m_ram[nParam + 0x00]);

			DECI2HANDLER* pHandler = GetDeci2Handler(nID);

			if(pHandler->nValid != 0)
			{
				uint32 stringAddr  = *reinterpret_cast<uint32*>(&m_ram[pHandler->nBufferAddr + 0x10]);
				stringAddr &= (PS2::EE_RAM_SIZE - 1);

				uint32 nLength = m_ram[stringAddr + 0x00] - 0x0C;
				uint8* sString = &m_ram[stringAddr + 0x0C];

				m_iopBios.GetIoman()->Write(1, nLength, sString);
			}

			m_ee.m_State.nGPR[SC_RETURN].nV[0] = 1;
			m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
		}
		break;
	case 0x04:
		//Deci2Poll
		{
			uint32 nID = *reinterpret_cast<uint32*>(&m_ram[nParam + 0x00]);
		
			DECI2HANDLER* pHandler = GetDeci2Handler(nID);
			if(pHandler->nValid != 0)
			{
				*(uint32*)&m_ram[pHandler->nBufferAddr + 0x0C] = 0;
			}

			m_ee.m_State.nGPR[SC_RETURN].nV[0] = 1;
			m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
		}
		break;
	case 0x10:
		//kPuts
		{
			uint32 stringAddr = *reinterpret_cast<uint32*>(&m_ram[nParam]);
			uint8* sString = &m_ram[stringAddr];
			m_iopBios.GetIoman()->Write(1, static_cast<uint32>(strlen(reinterpret_cast<char*>(sString))), sString);
		}
		break;
	default:
		CLog::GetInstance().Print(LOG_NAME, "Unknown Deci2Call function (0x%0.8X) called. PC: 0x%0.8X.\r\n", nFunction, m_ee.m_State.nPC);
		break;
	}

}

//7F
void CPS2OS::sc_GetMemorySize()
{
	m_ee.m_State.nGPR[SC_RETURN].nV[0] = PS2::EE_RAM_SIZE;
	m_ee.m_State.nGPR[SC_RETURN].nV[1] = 0;
}

//////////////////////////////////////////////////
//System Call Handler
//////////////////////////////////////////////////

void CPS2OS::SysCallHandler()
{

#ifdef PROFILE
	CProfiler::GetInstance().EndZone();
#endif

	uint32 searchAddress = m_ee.m_State.nCOP0[CCOP_SCU::EPC];
	uint32 callInstruction = m_ee.m_pMemoryMap->GetInstruction(searchAddress);
	if(callInstruction != 0x0000000C)
	{
		throw std::runtime_error("Not a SYSCALL.");
	}

	uint32 nFunc = m_ee.m_State.nGPR[3].nV[0];
	
	if(nFunc == 0x666)
	{
		//Reschedule
		ThreadShakeAndBake();
	}
	else
	{
		if(nFunc & 0x80000000)
		{
			nFunc = 0 - nFunc;
		}
		//Save for custom handler
		m_ee.m_State.nGPR[3].nV[0] = nFunc;

		if(GetCustomSyscallTable()[nFunc] == NULL)
		{
	#ifdef _DEBUG
			DisassembleSysCall(static_cast<uint8>(nFunc & 0xFF));
	#endif
			if(nFunc < 0x80)
			{
				((this)->*(m_pSysCall[nFunc & 0xFF]))();
			}
		}
		else
		{
			m_ee.GenerateException(0x1FC00100);
		}
	}

#ifdef PROFILE
	CProfiler::GetInstance().BeginZone(PROFILE_EEZONE);
#endif

	m_ee.m_State.nHasException = 0;
}

void CPS2OS::DisassembleSysCall(uint8 nFunc)
{
#ifdef _DEBUG
	std::string sDescription(GetSysCallDescription(nFunc));
	if(sDescription.length() != 0)
	{
		CLog::GetInstance().Print(LOG_NAME, "%d: %s\r\n", GetCurrentThreadId(), sDescription.c_str());
	}
#endif
}

std::string CPS2OS::GetSysCallDescription(uint8 nFunction)
{
	char sDescription[256];

	strcpy(sDescription, "");

	switch(nFunction)
	{
	case 0x02:
		sprintf(sDescription, "GsSetCrt(interlace = %i, mode = %i, field = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM2].nV[0]);
		break;
	case 0x06:
		sprintf(sDescription, SYSCALL_NAME_LOADEXECPS2 "(exec = 0x%0.8X, argc = %d, argv = 0x%0.8X);",
			m_ee.m_State.nGPR[SC_PARAM0].nV[0],
			m_ee.m_State.nGPR[SC_PARAM1].nV[0],
			m_ee.m_State.nGPR[SC_PARAM2].nV[0]);
		break;
	case 0x10:
		sprintf(sDescription, SYSCALL_NAME_ADDINTCHANDLER "(cause = %i, address = 0x%0.8X, next = 0x%0.8X, arg = 0x%0.8X);",
			m_ee.m_State.nGPR[SC_PARAM0].nV[0],
			m_ee.m_State.nGPR[SC_PARAM1].nV[0],
			m_ee.m_State.nGPR[SC_PARAM2].nV[0],
			m_ee.m_State.nGPR[SC_PARAM3].nV[0]);
		break;
	case 0x11:
		sprintf(sDescription, "RemoveIntcHandler(cause = %i, id = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x12:
		sprintf(sDescription, "AddDmacHandler(channel = %i, address = 0x%0.8X, next = %i, arg = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM2].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM3].nV[0]);
		break;
	case 0x13:
		sprintf(sDescription, "RemoveDmacHandler(channel = %i, handler = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x14:
		sprintf(sDescription, SYSCALL_NAME_ENABLEINTC "(cause = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x15:
		sprintf(sDescription, "DisableIntc(cause = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x16:
		sprintf(sDescription, "EnableDmac(channel = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x17:
		sprintf(sDescription, "DisableDmac(channel = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x20:
		sprintf(sDescription, SYSCALL_NAME_CREATETHREAD "(thread = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x21:
		sprintf(sDescription, "DeleteThread(id = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x22:
		sprintf(sDescription, SYSCALL_NAME_STARTTHREAD "(id = 0x%0.8X, a0 = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x23:
		sprintf(sDescription, "ExitThread();");
		break;
	case 0x25:
		sprintf(sDescription, "TerminateThread(id = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x29:
		sprintf(sDescription, "ChangeThreadPriority(id = 0x%0.8X, priority = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x2A:
		sprintf(sDescription, SYSCALL_NAME_ICHANGETHREADPRIORITY "(id = 0x%0.8X, priority = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x2B:
		sprintf(sDescription, SYSCALL_NAME_ROTATETHREADREADYQUEUE "(prio = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x2F:
		sprintf(sDescription, SYSCALL_NAME_GETTHREADID "();");
		break;
	case 0x30:
		sprintf(sDescription, SYSCALL_NAME_REFERTHREADSTATUS "(threadId = %d, infoPtr = 0x%0.8X);",
			m_ee.m_State.nGPR[SC_PARAM0].nV[0],
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x31:
		sprintf(sDescription, SYSCALL_NAME_IREFERTHREADSTATUS "(threadId = %d, infoPtr = 0x%0.8X);",
			m_ee.m_State.nGPR[SC_PARAM0].nV[0],
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x32:
		sprintf(sDescription, SYSCALL_NAME_SLEEPTHREAD "();");
		break;
	case 0x33:
		sprintf(sDescription, SYSCALL_NAME_WAKEUPTHREAD "(id = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x34:
		sprintf(sDescription, SYSCALL_NAME_IWAKEUPTHREAD "(id = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x37:
		sprintf(sDescription, SYSCALL_NAME_SUSPENDTHREAD "(id = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x39:
		sprintf(sDescription, SYSCALL_NAME_RESUMETHREAD "(id = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x3C:
		sprintf(sDescription, "SetupThread(gp = 0x%0.8X, stack = 0x%0.8X, stack_size = 0x%0.8X, args = 0x%0.8X, root_func = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM2].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM3].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM4].nV[0]);
		break;
	case 0x3D:
		sprintf(sDescription, "SetupHeap(heap_start = 0x%0.8X, heap_size = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x3E:
		sprintf(sDescription, SYSCALL_NAME_ENDOFHEAP "();");
		break;
	case 0x40:
		sprintf(sDescription, SYSCALL_NAME_CREATESEMA "(sema = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x41:
		sprintf(sDescription, SYSCALL_NAME_DELETESEMA "(semaid = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x42:
		sprintf(sDescription, SYSCALL_NAME_SIGNALSEMA "(semaid = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x43:
		sprintf(sDescription, SYSCALL_NAME_ISIGNALSEMA "(semaid = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x44:
		sprintf(sDescription, SYSCALL_NAME_WAITSEMA "(semaid = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x45:
		sprintf(sDescription, SYSCALL_NAME_POLLSEMA "(semaid = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x46:
		sprintf(sDescription, "iPollSema(semaid = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x47:
	case 0x48:
		sprintf(sDescription, "iReferSemaStatus(semaid = %i, status = 0x%0.8X);",
			m_ee.m_State.nGPR[SC_PARAM0].nV[0],
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x64:
	case 0x68:
#ifdef _DEBUG
//		sprintf(sDescription, SYSCALL_NAME_FLUSHCACHE "();");
#endif
		break;
	case 0x70:
		sprintf(sDescription, SYSCALL_NAME_GSGETIMR "();");
		break;
	case 0x71:
		sprintf(sDescription, SYSCALL_NAME_GSPUTIMR "(GS_IMR = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x73:
		sprintf(sDescription, "SetVSyncFlag(ptr1 = 0x%0.8X, ptr2 = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x74:
		sprintf(sDescription, "SetSyscall(num = 0x%0.2X, address = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x76:
		sprintf(sDescription, SYSCALL_NAME_SIFDMASTAT "();");
		break;
	case 0x77:
		sprintf(sDescription, SYSCALL_NAME_SIFSETDMA "(list = 0x%0.8X, count = %i);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x78:
		sprintf(sDescription, SYSCALL_NAME_SIFSETDCHAIN "();");
		break;
	case 0x79:
		sprintf(sDescription, "SifSetReg(register = 0x%0.8X, value = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x7A:
		sprintf(sDescription, "SifGetReg(register = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0]);
		break;
	case 0x7C:
		sprintf(sDescription, "Deci2Call(func = 0x%0.8X, param = 0x%0.8X);", \
			m_ee.m_State.nGPR[SC_PARAM0].nV[0], \
			m_ee.m_State.nGPR[SC_PARAM1].nV[0]);
		break;
	case 0x7F:
		sprintf(sDescription, "GetMemorySize();");
		break;
	}

	return std::string(sDescription);
}

//////////////////////////////////////////////////
//System Call Handlers Table
//////////////////////////////////////////////////

CPS2OS::SystemCallHandler CPS2OS::m_pSysCall[0x80] =
{
	//0x00
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_GsSetCrt,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_LoadExecPS2,	&CPS2OS::sc_Unhandled,
	//0x08
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x10
	&CPS2OS::sc_AddIntcHandler,		&CPS2OS::sc_RemoveIntcHandler,		&CPS2OS::sc_AddDmacHandler,			&CPS2OS::sc_RemoveDmacHandler,		&CPS2OS::sc_EnableIntc,		&CPS2OS::sc_DisableIntc,	&CPS2OS::sc_EnableDmac,		&CPS2OS::sc_DisableDmac,
	//0x18
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x20
	&CPS2OS::sc_CreateThread,		&CPS2OS::sc_DeleteThread,			&CPS2OS::sc_StartThread,			&CPS2OS::sc_ExitThread,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_TerminateThread,&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x28
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_ChangeThreadPriority,	&CPS2OS::sc_ChangeThreadPriority,	&CPS2OS::sc_RotateThreadReadyQueue,	&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_GetThreadId,
	//0x30
	&CPS2OS::sc_ReferThreadStatus,	&CPS2OS::sc_ReferThreadStatus,		&CPS2OS::sc_SleepThread,			&CPS2OS::sc_WakeupThread,			&CPS2OS::sc_WakeupThread,	&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_SuspendThread,
	//0x38
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_ResumeThread,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_SetupThread,	&CPS2OS::sc_SetupHeap,		&CPS2OS::sc_EndOfHeap,		&CPS2OS::sc_Unhandled,
	//0x40
	&CPS2OS::sc_CreateSema,			&CPS2OS::sc_DeleteSema,				&CPS2OS::sc_SignalSema,				&CPS2OS::sc_SignalSema,				&CPS2OS::sc_WaitSema,		&CPS2OS::sc_PollSema,		&CPS2OS::sc_PollSema,		&CPS2OS::sc_ReferSemaStatus,
	//0x48
	&CPS2OS::sc_ReferSemaStatus,	&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x50
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x58
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x60
	&CPS2OS::sc_Unhandled,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_FlushCache,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x68
	&CPS2OS::sc_FlushCache,			&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,
	//0x70
	&CPS2OS::sc_GsGetIMR,			&CPS2OS::sc_GsPutIMR,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_SetVSyncFlag,			&CPS2OS::sc_SetSyscall,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_SifDmaStat,		&CPS2OS::sc_SifSetDma,
	//0x78
	&CPS2OS::sc_SifSetDChain,		&CPS2OS::sc_SifSetReg,				&CPS2OS::sc_SifGetReg,				&CPS2OS::sc_Unhandled,				&CPS2OS::sc_Deci2Call,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_Unhandled,		&CPS2OS::sc_GetMemorySize,
};

//////////////////////////////////////////////////
//Round Ribbon Implementation
//////////////////////////////////////////////////

CPS2OS::CRoundRibbon::CRoundRibbon(void* pMemory, uint32 nSize)
: m_pNode(reinterpret_cast<NODE*>(pMemory))
, m_nMaxNode(nSize / sizeof(NODE))
{
	memset(pMemory, 0, nSize);

	NODE* pHead = GetNode(0);
	pHead->nIndexNext	= -1;
	pHead->nWeight		= -1;
	pHead->nValid		= 1;
}

CPS2OS::CRoundRibbon::~CRoundRibbon()
{

}

unsigned int CPS2OS::CRoundRibbon::Insert(uint32 nValue, uint32 nWeight)
{
	//Initialize the new node
	NODE* pNode = AllocateNode();
	if(pNode == NULL) return -1;
	pNode->nWeight	= nWeight;
	pNode->nValue	= nValue;

	//Insert node in list
	NODE* pNext = GetNode(0);
	NODE* pPrev = NULL;

	while(1)
	{
		if(pNext == NULL)
		{
			//We must insert there...
			pNode->nIndexNext = pPrev->nIndexNext;
			pPrev->nIndexNext = GetNodeIndex(pNode);
			break;
		}

		if(pNext->nWeight == -1)
		{
			pPrev = pNext;
			pNext = GetNode(pNext->nIndexNext);
			continue;
		}

		if(pNode->nWeight < pNext->nWeight)
		{
			pNext = NULL;
			continue;
		}

		pPrev = pNext;
		pNext = GetNode(pNext->nIndexNext);
	}

	return GetNodeIndex(pNode);
}

void CPS2OS::CRoundRibbon::Remove(unsigned int nIndex)
{
	if(nIndex == 0) return;

	NODE* pCurr = GetNode(nIndex);
	if(pCurr == NULL) return;
	if(pCurr->nValid != 1) return;

	NODE* pNode = GetNode(0);

	while(1)
	{
		if(pNode == NULL) break;
		assert(pNode->nValid);

		if(pNode->nIndexNext == nIndex)
		{
			pNode->nIndexNext = pCurr->nIndexNext;
			break;
		}
		
		pNode = GetNode(pNode->nIndexNext);
	}

	FreeNode(pCurr);
}

unsigned int CPS2OS::CRoundRibbon::Begin()
{
	return GetNode(0)->nIndexNext;
}

CPS2OS::CRoundRibbon::NODE* CPS2OS::CRoundRibbon::GetNode(unsigned int nIndex)
{
	if(nIndex >= m_nMaxNode) return NULL;
	return m_pNode + nIndex;
}

unsigned int CPS2OS::CRoundRibbon::GetNodeIndex(NODE* pNode)
{
	return (unsigned int)(pNode - m_pNode);
}

CPS2OS::CRoundRibbon::NODE* CPS2OS::CRoundRibbon::AllocateNode()
{
	for(unsigned int i = 1; i < m_nMaxNode; i++)
	{
		NODE* pNode = GetNode(i);
		if(pNode->nValid == 1) continue;
		pNode->nValid = 1;
		return pNode;
	}

	return NULL;
}

void CPS2OS::CRoundRibbon::FreeNode(NODE* pNode)
{
	pNode->nValid = 0;
}

CPS2OS::CRoundRibbon::ITERATOR::ITERATOR(CRoundRibbon* pRibbon)
: m_pRibbon(pRibbon)
, m_nIndex(0)
{

}

CPS2OS::CRoundRibbon::ITERATOR& CPS2OS::CRoundRibbon::ITERATOR::operator =(unsigned int nIndex)
{
	m_nIndex = nIndex;
	return (*this);
}

CPS2OS::CRoundRibbon::ITERATOR& CPS2OS::CRoundRibbon::ITERATOR::operator ++(int nAmount)
{
	if(!IsEnd())
	{
		NODE* pNode = m_pRibbon->GetNode(m_nIndex);
		m_nIndex = pNode->nIndexNext;
	}

	return (*this);
}

uint32 CPS2OS::CRoundRibbon::ITERATOR::GetValue()
{
	if(!IsEnd())
	{
		return m_pRibbon->GetNode(m_nIndex)->nValue;
	}

	return 0;
}

uint32 CPS2OS::CRoundRibbon::ITERATOR::GetWeight()
{
	if(!IsEnd())
	{
		return m_pRibbon->GetNode(m_nIndex)->nWeight;
	}

	return -1;
}

unsigned int CPS2OS::CRoundRibbon::ITERATOR::GetIndex()
{
	return m_nIndex;
}

bool CPS2OS::CRoundRibbon::ITERATOR::IsEnd()
{
	if(m_pRibbon == NULL) return true;
	return m_pRibbon->GetNode(m_nIndex) == NULL;
}
