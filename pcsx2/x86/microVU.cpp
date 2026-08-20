// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "microVU.h"

#include "common/AlignedMalloc.h"
#include "common/Perf.h"
#include "common/StringUtil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

//------------------------------------------------------------------
// Micro VU - Main Functions
//------------------------------------------------------------------

// Only run this once per VU! ;)
void mVUinit(microVU& mVU, uint vuIndex)
{
	std::memset(&mVU.prog, 0, sizeof(mVU.prog));

	mVU.index        =  vuIndex;
	mVU.cop2         =  0;
	mVU.vuMemSize    = (mVU.index ? 0x4000 : 0x1000);
	mVU.microMemSize = (mVU.index ? 0x4000 : 0x1000);
	mVU.progSize     = (mVU.index ? 0x4000 : 0x1000) / 4;
	mVU.progMemMask  =  mVU.progSize-1;
	mVU.cache        = vuIndex ? SysMemory::GetVU1Rec() : SysMemory::GetVU0Rec();
	mVU.prog.x86end  = (vuIndex ? SysMemory::GetVU1RecEnd() : SysMemory::GetVU0RecEnd()) - (mVUcacheSafeZone * _1mb);

	mVU.regAlloc.reset(new microRegAlloc(mVU.index));
}

// Resets Rec Data
void mVUreset(microVU& mVU, bool resetReserve)
{
	if (THREAD_VU1)
	{
		DevCon.Warning("mVU Reset");
		// If MTVU is toggled on during gameplay we need to flush the running VU1 program, else it gets in a mess
		if (VU0.VI[REG_VPU_STAT].UL & 0x100)
		{
			CpuVU1->Execute(vu1RunCycles);
		}
		VU0.VI[REG_VPU_STAT].UL &= ~0x100;
	}

	xSetPtr(mVU.cache);
	mVUdispatcherAB(mVU);
	mVUdispatcherCD(mVU);
	mVUGenerateWaitMTVU(mVU);
	mVUGenerateCopyPipelineState(mVU);
	mVUGenerateCompareState(mVU);

	mVU.regs().nextBlockCycles = 0;
	memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));
	mVU.profiler.Reset(mVU.index);

	// Program Variables
	mVU.prog.cleared  =  1;
	mVU.prog.isSame   = -1;
	mVU.prog.cur      = NULL;
	mVU.prog.total    =  0;
	mVU.prog.curFrame =  0;

	// Setup Dynarec Cache Limits for Each Program
	mVU.prog.x86start = xGetAlignedCallTarget();
	mVU.prog.x86ptr   = mVU.prog.x86start;

	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
		{
			mVU.prog.prog[i] = new std::deque<microProgram*>();
			continue;
		}
		std::deque<microProgram*>::iterator it(mVU.prog.prog[i]->begin());
		for (; it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		mVU.prog.prog[i]->clear();
		mVU.prog.quick[i].block = NULL;
		mVU.prog.quick[i].prog = NULL;
	}
}

// Free Allocated Resources
void mVUclose(microVU& mVU)
{
	// Delete Programs and Block Managers
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
			continue;
		std::deque<microProgram*>::iterator it(mVU.prog.prog[i]->begin());
		for (; it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		safe_delete(mVU.prog.prog[i]);
	}
}

// Clears Block Data in specified range
__fi void mVUclear(mV, u32 addr, u32 size)
{
	if (!mVU.prog.cleared)
	{
		mVU.prog.cleared = 1; // Next execution searches/creates a new microprogram
		std::memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState)); // Clear pipeline state
		for (u32 i = 0; i < (mVU.progSize / 2); i++)
		{
			mVU.prog.quick[i].block = NULL; // Clear current quick-reference block
			mVU.prog.quick[i].prog = NULL; // Clear current quick-reference prog
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Private Functions
//------------------------------------------------------------------

// Deletes a program
__ri void mVUdeleteProg(microVU& mVU, microProgram*& prog)
{
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		safe_delete(prog->block[i]);
	}
	safe_delete(prog->ranges);
	safe_aligned_free(prog);
}

// Creates a new Micro Program
__ri microProgram* mVUcreateProg(microVU& mVU, int startPC)
{
	microProgram* prog = (microProgram*)_aligned_malloc(sizeof(microProgram), 64);
	memset(prog, 0, sizeof(microProgram));
	prog->idx = mVU.prog.total++;
	prog->ranges = new std::deque<microRange>();
	prog->startPC = startPC;
	if(doWholeProgCompare)
		mVUcacheProg(mVU, *prog); // Cache Micro Program
	double cacheSize = (double)((uptr)mVU.prog.x86end - (uptr)mVU.prog.x86start);
	double cacheUsed = ((double)((uptr)mVU.prog.x86ptr - (uptr)mVU.prog.x86start)) / (double)_1mb;
	double cachePerc = ((double)((uptr)mVU.prog.x86ptr - (uptr)mVU.prog.x86start)) / cacheSize * 100;
	ConsoleColors c = mVU.index ? Color_Orange : Color_Magenta;
	DevCon.WriteLn(c, "microVU%d: Cached Prog = [%03d] [PC=%04x] [List=%02d] (Cache=%3.3f%%) [%3.1fmb]",
		mVU.index, prog->idx, startPC * 8, mVU.prog.prog[startPC]->size() + 1, cachePerc, cacheUsed);
	return prog;
}

// Caches Micro Program
__ri void mVUcacheProg(microVU& mVU, microProgram& prog)
{
	if (!doWholeProgCompare)
	{
		auto cmpOffset = [&](void* x) { return (u8*)x + mVUrange.start; };
		memcpy(cmpOffset(prog.data), cmpOffset(mVU.regs().Micro), (mVUrange.end - mVUrange.start));
	}
	else
	{
		if (!mVU.index)
			memcpy(prog.data, mVU.regs().Micro, 0x1000);
		else
			memcpy(prog.data, mVU.regs().Micro, 0x4000);
	}
	mVUdumpProg(mVU, prog);
}

// Generate Hash for partial program based on compiled ranges...
u64 mVUrangesHash(microVU& mVU, microProgram& prog)
{
	union
	{
		u64 v64;
		u32 v32[2];
	} hash = {0};

	std::deque<microRange>::const_iterator it(prog.ranges->begin());
	for (; it != prog.ranges->end(); ++it)
	{
		if ((it[0].start < 0) || (it[0].end < 0))
		{
			DevCon.Error("microVU%d: Negative Range![%d][%d]", mVU.index, it[0].start, it[0].end);
		}
		for (int i = it[0].start / 4; i < it[0].end / 4; i++)
		{
			hash.v32[0] -= prog.data[i];
			hash.v32[1] ^= prog.data[i];
		}
	}
	return hash.v64;
}

// Prints the ratio of unique programs to total programs
void mVUprintUniqueRatio(microVU& mVU)
{
	std::vector<u64> v;
	for (u32 pc = 0; pc < mProgSize / 2; pc++)
	{
		microProgramList* list = mVU.prog.prog[pc];
		if (!list)
			continue;
		std::deque<microProgram*>::iterator it(list->begin());
		for (; it != list->end(); ++it)
		{
			v.push_back(mVUrangesHash(mVU, *it[0]));
		}
	}
	u32 total = v.size();
	sortVector(v);
	makeUnique(v);
	if (!total)
		return;
	DevCon.WriteLn("%d / %d [%3.1f%%]", v.size(), total, 100. - (double)v.size() / (double)total * 100.);
}

// Compare Cached microProgram to mVU.regs().Micro
__fi bool mVUcmpProg(microVU& mVU, microProgram& prog)
{
	if (doWholeProgCompare)
	{
		if (memcmp((u8*)prog.data, mVU.regs().Micro, mVU.microMemSize))
			return false;
	}
	else
	{
		for (const auto& range : *prog.ranges)
		{
#if defined(PCSX2_DEVBUILD) || defined(_DEBUG)
			if ((range.start < 0) || (range.end < 0))
				DevCon.Error("microVU%d: Negative Range![%d][%d]", mVU.index, range.start, range.end);
#endif
			auto cmpOffset = [&](void* x) { return (u8*)x + range.start; };

			if (memcmp(cmpOffset(prog.data), cmpOffset(mVU.regs().Micro), (range.end - range.start)))
				return false;
		}
	}
	mVU.prog.cleared = 0;
	mVU.prog.cur = &prog;
	mVU.prog.isSame = doWholeProgCompare ? 1 : -1;
	return true;
}

// Searches for Cached Micro Program and sets prog.cur to it (returns entry-point to program)
_mVUt __fi void* mVUsearchProg(u32 startPC, uptr pState)
{
	microVU& mVU = mVUx;
	microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
	microProgramList*  list  = mVU.prog.prog [mVU.regs().start_pc / 8];

	if (!quick.prog) // If null, we need to search for new program
	{
		std::deque<microProgram*>::iterator it(list->begin());
		for (; it != list->end(); ++it)
		{
			bool b = mVUcmpProg(mVU, *it[0]);

			if (b)
			{
				quick.block = it[0]->block[startPC / 8];
				quick.prog  = it[0];
				list->erase(it);
				list->push_front(quick.prog);

				// Sanity check, in case for some reason the program compilation aborted half way through (JALR for example)
				if (quick.block == nullptr)
				{
					void* entryPoint = mVUblockFetch(mVU, startPC, pState);
					return entryPoint;
				}
				return mVUentryGet(mVU, quick.block, startPC, pState);
			}
		}

		// If cleared and program not found, make a new program instance
		mVU.prog.cleared = 0;
		mVU.prog.isSame  = 1;
		mVU.prog.cur     = mVUcreateProg(mVU, mVU.regs().start_pc/8);
		void* entryPoint = mVUblockFetch(mVU,  startPC, pState);
		quick.block      = mVU.prog.cur->block[startPC/8];
		quick.prog       = mVU.prog.cur;
		list->push_front(mVU.prog.cur);
		//mVUprintUniqueRatio(mVU);
		return entryPoint;
	}

	// If list.quick, then we've already found and recompiled the program ;)
	mVU.prog.isSame = -1;
	mVU.prog.cur = quick.prog;
	// Because the VU's can now run in sections and not whole programs at once
	// we need to set the current block so it gets the right program back
	quick.block = mVU.prog.cur->block[startPC / 8];

	// Sanity check, in case for some reason the program compilation aborted half way through
	if (quick.block == nullptr)
	{
		void* entryPoint = mVUblockFetch(mVU, startPC, pState);
		return entryPoint;
	}
	return mVUentryGet(mVU, quick.block, startPC, pState);
}

//------------------------------------------------------------------
// recMicroVU0 / recMicroVU1
//------------------------------------------------------------------

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;

recMicroVU0::recMicroVU0() { m_Idx = 0; IsInterpreter = false; }
recMicroVU1::recMicroVU1() { m_Idx = 1; IsInterpreter = false; }

void recMicroVU0::Reserve()
{
	mVUinit(microVU0, 0);
}
void recMicroVU1::Reserve()
{
	mVUinit(microVU1, 1);
	vu1Thread.Open();
}

void recMicroVU0::Shutdown()
{
	mVUclose(microVU0);
}
void recMicroVU1::Shutdown()
{
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	mVUclose(microVU1);
}

void recMicroVU0::Reset()
{
	mVUreset(microVU0, true);
}

void recMicroVU0::Step()
{
}

void recMicroVU1::Reset()
{
	vu1Thread.WaitVU();
	vu1Thread.Get_MTVUChanges();
	mVUreset(microVU1, true);
}

void recMicroVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void recMicroVU0::Execute(u32 cycles)
{
	VU0.flags &= ~VUFLAG_MFLAGSET;

	if (!(VU0.VI[REG_VPU_STAT].UL & 1))
		return;
	VU0.VI[REG_TPC].UL <<= 3;

	((mVUrecCall)microVU0.startFunct)(VU0.VI[REG_TPC].UL, cycles);
	VU0.VI[REG_TPC].UL >>= 3;
	if (microVU0.regs().flags & 0x4)
	{
		microVU0.regs().flags &= ~0x4;
		hwIntcIrq(6);
	}
}

void recMicroVU1::SetStartPC(u32 startPC)
{
	VU1.start_pc = startPC;
}

void recMicroVU1::Step()
{
}

void recMicroVU1::Execute(u32 cycles)
{
	if (!THREAD_VU1)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
			return;
	}
	VU1.VI[REG_TPC].UL <<= 3;
	((mVUrecCall)microVU1.startFunct)(VU1.VI[REG_TPC].UL, cycles);
	VU1.VI[REG_TPC].UL >>= 3;
	if (microVU1.regs().flags & 0x4 && !THREAD_VU1)
	{
		microVU1.regs().flags &= ~0x4;
		hwIntcIrq(7);
	}
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
	mVUclear(microVU0, addr, size);
}
void recMicroVU1::Clear(u32 addr, u32 size)
{
	mVUclear(microVU1, addr, size);
}

void recMicroVU1::ResumeXGkick()
{
	if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
		return;
	((mVUrecCallXG)microVU1.startFunctXG)();
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Freeze(microVU0.prog.lpState);
	Freeze(microVU1.prog.lpState);
	return IsOkay();
}

static float Torneko3RawToFloat(u32 v)
{
	float f;
	std::memcpy(&f, &v, sizeof(f));
	return f;
}

static bool Torneko3MakeCaptureDir(const char* path)
{
#ifdef _WIN32
	return _mkdir(path) == 0 || errno == EEXIST;
#else
	return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

static const char* Torneko3CaptureDir()
{
	const char* env = std::getenv("TORNEKO3_VU1_CAPTURE_DIR");
	const char* dir = (env && env[0]) ? env : "C:\\Users\\asdtr\\torneko3_vu1_backend_hook_20260820";
	Torneko3MakeCaptureDir(dir);
	return dir;
}

static bool Torneko3TargetSignatureMatches()
{
	const u32* q00a = reinterpret_cast<const u32*>(&vuRegs[1].Mem[0x00a * 16]);
	return q00a[0] == 0x3ee5e354 && q00a[1] == 0x400d6042 && q00a[2] == 0x4026d917;
}

static bool& Torneko3CapturedFlag(u32 pc)
{
	static bool pc0818 = false;
	static bool pc0820 = false;
	static bool pc0838 = false;
	static bool pc0840 = false;
	static bool unknown = true;

	switch (pc)
	{
		case 0x0818: return pc0818;
		case 0x0820: return pc0820;
		case 0x0838: return pc0838;
		case 0x0840: return pc0840;
		default: return unknown;
	}
}

static const char* Torneko3CaptureName(u32 pc)
{
	switch (pc)
	{
		case 0x0818: return "xtop_pc0818";
		case 0x0820: return "xtop_pc0820";
		case 0x0838: return "target_pc0838";
		case 0x0840: return "target_pc0840";
		default: return "unknown_pc";
	}
}

static void Torneko3WriteVectorJson(std::FILE* fp, const char* name, const VECTOR& v)
{
	std::fprintf(fp, "    \"%s\": {\"raw\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"], \"float\": [%.9g, %.9g, %.9g, %.9g]}",
		name, v.UL[0], v.UL[1], v.UL[2], v.UL[3], v.F[0], v.F[1], v.F[2], v.F[3]);
}

static void Torneko3WriteMemQwordJson(std::FILE* fp, const char* name, u32 qaddr)
{
	const u32* q = reinterpret_cast<const u32*>(&vuRegs[1].Mem[(qaddr & 0x3ff) * 16]);
	std::fprintf(fp, "    \"%s\": {\"qword\": \"0x%03x\", \"raw\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"], \"float\": [%.9g, %.9g, %.9g, %.9g]}",
		name, qaddr, q[0], q[1], q[2], q[3], Torneko3RawToFloat(q[0]), Torneko3RawToFloat(q[1]), Torneko3RawToFloat(q[2]), Torneko3RawToFloat(q[3]));
}

void Torneko3DumpTargetVU1State(u32 pc)
{
	bool& captured = Torneko3CapturedFlag(pc);
	if (captured || !Torneko3TargetSignatureMatches())
		return;
	captured = true;

	const char* dir = Torneko3CaptureDir();
	const char* name = Torneko3CaptureName(pc);
	char json_path[512];
	char mem_path[512];
#ifdef _WIN32
	std::snprintf(json_path, sizeof(json_path), "%s\\%s_regs.json", dir, name);
	std::snprintf(mem_path, sizeof(mem_path), "%s\\%s_vumem.bin", dir, name);
#else
	std::snprintf(json_path, sizeof(json_path), "%s/%s_regs.json", dir, name);
	std::snprintf(mem_path, sizeof(mem_path), "%s/%s_vumem.bin", dir, name);
#endif

	if (std::FILE* mem = std::fopen(mem_path, "wb"))
	{
		std::fwrite(vuRegs[1].Mem, 1, 0x4000, mem);
		std::fclose(mem);
	}

	std::FILE* fp = std::fopen(json_path, "wb");
	if (!fp)
		return;

	const VURegs& r = vuRegs[1];
	const microVU& m = microVU1;
	std::fprintf(fp, "{\n");
	std::fprintf(fp, "  \"pc_before\": \"0x%04x\",\n", pc);
	std::fprintf(fp, "  \"target_signature\": {\"q00a_xyz_raw\": [\"0x3ee5e354\", \"0x400d6042\", \"0x4026d917\"]},\n");
	std::fprintf(fp, "  \"vu1_memory_file\": \"%s_vumem.bin\",\n", name);
	std::fprintf(fp, "  \"qwords\": {\n");
	Torneko3WriteMemQwordJson(fp, "q0x008", 0x008); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x009", 0x009); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x00a", 0x00a); std::fprintf(fp, "\n");
	std::fprintf(fp, "  },\n");
	std::fprintf(fp, "  \"vi\": {\n");
	for (u32 i = 0; i < 32; i++)
	{
		std::fprintf(fp, "    \"vi%u\": {\"raw\": \"0x%08x\", \"u32\": %u}%s\n", i, r.VI[i].UL, r.VI[i].UL, (i == 31) ? "" : ",");
	}
	std::fprintf(fp, "  },\n");
	std::fprintf(fp, "  \"vf\": {\n");
	for (u32 i = 0; i < 32; i++)
	{
		char key[16];
		std::snprintf(key, sizeof(key), "vf%u", i);
		Torneko3WriteVectorJson(fp, key, r.VF[i]);
		std::fprintf(fp, "%s\n", (i == 31) ? "" : ",");
	}
	std::fprintf(fp, "  },\n");
	std::fprintf(fp, "  \"special\": {\n");
	Torneko3WriteVectorJson(fp, "acc", r.ACC); std::fprintf(fp, ",\n");
	std::fprintf(fp, "    \"q\": {\"raw\": \"0x%08x\", \"float\": %.9g},\n", r.q.UL, r.q.F);
	std::fprintf(fp, "    \"p\": {\"raw\": \"0x%08x\", \"float\": %.9g},\n", r.p.UL, r.p.F);
	std::fprintf(fp, "    \"pending_q\": \"0x%08x\",\n", r.pending_q);
	std::fprintf(fp, "    \"pending_p\": \"0x%08x\"\n", r.pending_p);
	std::fprintf(fp, "  },\n");
	std::fprintf(fp, "  \"flags\": {\n");
	std::fprintf(fp, "    \"macflag\": \"0x%08x\", \"statusflag\": \"0x%08x\", \"clipflag\": \"0x%08x\",\n", r.macflag, r.statusflag, r.clipflag);
	std::fprintf(fp, "    \"micro_macflags\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n", r.micro_macflags[0], r.micro_macflags[1], r.micro_macflags[2], r.micro_macflags[3]);
	std::fprintf(fp, "    \"micro_clipflags\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n", r.micro_clipflags[0], r.micro_clipflags[1], r.micro_clipflags[2], r.micro_clipflags[3]);
	std::fprintf(fp, "    \"micro_statusflags\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n", r.micro_statusflags[0], r.micro_statusflags[1], r.micro_statusflags[2], r.micro_statusflags[3]);
	std::fprintf(fp, "    \"mvu_macFlag\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n", m.macFlag[0], m.macFlag[1], m.macFlag[2], m.macFlag[3]);
	std::fprintf(fp, "    \"mvu_clipFlag\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n", m.clipFlag[0], m.clipFlag[1], m.clipFlag[2], m.clipFlag[3]);
	std::fprintf(fp, "    \"mvu_statFlag\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"]\n", m.statFlag[0], m.statFlag[1], m.statFlag[2], m.statFlag[3]);
	std::fprintf(fp, "  }\n");
	std::fprintf(fp, "}\n");
	std::fclose(fp);
}

#if 0

#include <zlib.h>

void DumpVUState(u32 n, u32 pc)
{
	const VURegs& r = vuRegs[n];
	const microVU& mVU = (n == 0) ? microVU0 : microVU1;
	static FILE* fp = nullptr;
	static bool fp_opened = false;
	static u32 counter = 0;

	u32 first = pc >> 31;
	pc &= 0x7FFFFFFFu;
	if (first)
		counter++;

#if 0
	if (counter == 184639 && pc == 0x0D70)
		__debugbreak();
#endif

	if (counter < 0)
		return;

	if (!fp_opened)
	{
		fp = std::fopen("C:\\Dumps\\comp\\vulog.txt", "wb");
		fp_opened = true;
	}
	if (fp)
	{
		const microVU& m = (n == 0) ? microVU0 : microVU1;
		fprintf(fp, "%08d VU%u SPC:%04X xPC:%04X BRANCH:%04X VIBACKUP:%04X", counter, n, r.start_pc, pc, mVU.branch, mVU.VIbackup);
#if 1
		//fprintf(fp, " MEM:%08X", crc32(0, (Bytef*)r.Mem, (n == 0) ? VU0_MEMSIZE : VU1_MEMSIZE));
		fprintf(fp, " MAC %08X %08X %08X %08X [%08X %08X %08X %08X]", r.micro_macflags[3], r.micro_macflags[2], r.micro_macflags[1], r.micro_macflags[0], m.macFlag[3], m.macFlag[2], m.macFlag[1], m.macFlag[0]);
		fprintf(fp, " CLIP %08X %08X %08X %08X [%08X %08X %08X %08X]", r.micro_clipflags[3], r.micro_clipflags[2], r.micro_clipflags[1], r.micro_clipflags[0], m.clipFlag[3], m.clipFlag[2], m.clipFlag[1], m.clipFlag[0]);
		fprintf(fp, " STATUS %08X %08X %08X %08X [%08X %08X %08X %08X]", r.micro_statusflags[3], r.micro_statusflags[2], r.micro_statusflags[1], r.micro_statusflags[0], m.statFlag[3], m.statFlag[2], m.statFlag[1], m.statFlag[0]);

		for (u32 i = 0; i < 32; i++)
		{
			const VECTOR& v = r.VF[i];
			fprintf(fp, " VF%u: %08X%08X%08X%08X (%f,%f,%f,%f)", i, v.UL[3], v.UL[2], v.UL[1], v.UL[0], v.F[3], v.F[2], v.F[1], v.F[0]);
		}

		for (u32 i = 0; i < 32; i++)
		{
			const REG_VI& v = r.VI[i];
			fprintf(fp, " VI%u: %08X", i, v.UL);
		}

		fprintf(fp, " ACC: %08X%08X%08X%08X (%f,%f,%f,%f)", r.ACC.UL[3], r.ACC.UL[2], r.ACC.UL[1], r.ACC.UL[0],
			r.ACC.F[3], r.ACC.F[2], r.ACC.F[1], r.ACC.F[0]);
		fprintf(fp, " Q: %08X (%f)", r.q.UL, r.q.F);
		fprintf(fp, " P: %08X (%f)\n", r.p.UL, r.p.F);
#else
		fprintf(fp, " REG:%08X\n", crc32(0, (Bytef*)&r, offsetof(VURegs, idx)));
#endif
		//fflush(fp);
	}
}

#endif
