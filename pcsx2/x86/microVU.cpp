// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "microVU.h"

#include "common/AlignedMalloc.h"
#include "common/Perf.h"
#include "common/StringUtil.h"

#include "vtlb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <string>
#include <vector>

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
	const char* dir = (env && env[0]) ? env : "C:\\Users\\asdtr\\torneko3_strip0_runtime_transform_20260820";
	Torneko3MakeCaptureDir(dir);
	return dir;
}

struct Torneko3CaptureConfig
{
	bool loaded = false;
	char dir[260] = "C:\\Users\\asdtr\\torneko3_strip0_runtime_transform_20260820";
	u32 pcs[64] = {0x0340};
	u32 pc_count = 1;
	bool require_q00a_xyz = true;
	u32 q00a_xyz[3] = {0x3ee5e354, 0x400d6042, 0x4026d917};
	bool require_vi4 = false;
	u32 vi4 = 0x0000;
	bool require_vi5 = true;
	u32 vi5 = 0x0088;
	bool require_gif_tag = false;
	u32 gif_qword = 0x087;
	u32 gif_nloop = 37;
	u32 gif_flg = 0;
	u32 gif_nreg = 3;
	u64 gif_regs_mask = 0xfffull;
	u64 gif_regs_value = 0x412ull;
	u32 max_captures_per_pc = 16;
	char ini_path[512] = {};
};

static char* Torneko3Trim(char* s)
{
	while (*s && std::isspace(static_cast<unsigned char>(*s)))
		s++;
	char* end = s + std::strlen(s);
	while (end > s && std::isspace(static_cast<unsigned char>(end[-1])))
		*--end = 0;
	return s;
}

static bool Torneko3ParseBool(const char* s, bool def)
{
	if (!s || !s[0])
		return def;
	return std::strcmp(s, "1") == 0 || StringUtil::Strcasecmp(s, "true") == 0 || StringUtil::Strcasecmp(s, "yes") == 0 || StringUtil::Strcasecmp(s, "on") == 0;
}

static u32 Torneko3ParseU32(const char* s, u32 def)
{
	if (!s || !s[0])
		return def;
	return static_cast<u32>(std::strtoul(s, nullptr, 0));
}

static u64 Torneko3ParseU64(const char* s, u64 def)
{
	if (!s || !s[0])
		return def;
	return static_cast<u64>(std::strtoull(s, nullptr, 0));
}

static void Torneko3ParsePcList(Torneko3CaptureConfig& cfg, char* value)
{
	constexpr u32 MAX_PCS = sizeof(cfg.pcs) / sizeof(cfg.pcs[0]);
	cfg.pc_count = 0;
	for (char* tok = std::strtok(value, ", \t"); tok && cfg.pc_count < MAX_PCS; tok = std::strtok(nullptr, ", \t"))
		cfg.pcs[cfg.pc_count++] = Torneko3ParseU32(tok, 0);
	if (cfg.pc_count == 0)
	{
		cfg.pcs[0] = 0x0340;
		cfg.pc_count = 1;
	}
}

static const Torneko3CaptureConfig& Torneko3Config()
{
	static Torneko3CaptureConfig cfg;
	if (cfg.loaded)
		return cfg;
	cfg.loaded = true;

	const char* dir_env = std::getenv("TORNEKO3_VU1_CAPTURE_DIR");
	if (dir_env && dir_env[0])
		std::snprintf(cfg.dir, sizeof(cfg.dir), "%s", dir_env);

	const char* pcs_env = std::getenv("TORNEKO3_VU1_CAPTURE_PCS");
	if (pcs_env && pcs_env[0])
	{
		char tmp[512];
		std::snprintf(tmp, sizeof(tmp), "%s", pcs_env);
		Torneko3ParsePcList(cfg, tmp);
	}

	char ini_path[512];
	const char* ini_env = std::getenv("TORNEKO3_VU1_CAPTURE_INI");
	if (ini_env && ini_env[0])
	{
		std::snprintf(ini_path, sizeof(ini_path), "%s", ini_env);
	}
	else
	{
#ifdef _WIN32
		std::snprintf(ini_path, sizeof(ini_path), "%s\\torneko3_vu1_capture.ini", cfg.dir);
#else
		std::snprintf(ini_path, sizeof(ini_path), "%s/torneko3_vu1_capture.ini", cfg.dir);
#endif
	}

	if (std::FILE* fp = std::fopen(ini_path, "rb"))
	{
		char line[1024];
		while (std::fgets(line, sizeof(line), fp))
		{
			char* comment = std::strpbrk(line, "#;");
			if (comment)
				*comment = 0;
			char* eq = std::strchr(line, '=');
			if (!eq)
				continue;
			*eq = 0;
			char* key = Torneko3Trim(line);
			char* value = Torneko3Trim(eq + 1);
			if (!key[0])
				continue;

			if (StringUtil::Strcasecmp(key, "capture_dir") == 0)
				std::snprintf(cfg.dir, sizeof(cfg.dir), "%s", value);
			else if (StringUtil::Strcasecmp(key, "pcs") == 0 || StringUtil::Strcasecmp(key, "capture_pcs") == 0)
				Torneko3ParsePcList(cfg, value);
			else if (StringUtil::Strcasecmp(key, "require_q00a_xyz") == 0)
				cfg.require_q00a_xyz = Torneko3ParseBool(value, cfg.require_q00a_xyz);
			else if (StringUtil::Strcasecmp(key, "q00a_x") == 0)
				cfg.q00a_xyz[0] = Torneko3ParseU32(value, cfg.q00a_xyz[0]);
			else if (StringUtil::Strcasecmp(key, "q00a_y") == 0)
				cfg.q00a_xyz[1] = Torneko3ParseU32(value, cfg.q00a_xyz[1]);
			else if (StringUtil::Strcasecmp(key, "q00a_z") == 0)
				cfg.q00a_xyz[2] = Torneko3ParseU32(value, cfg.q00a_xyz[2]);
			else if (StringUtil::Strcasecmp(key, "require_vi4") == 0)
				cfg.require_vi4 = Torneko3ParseBool(value, cfg.require_vi4);
			else if (StringUtil::Strcasecmp(key, "vi4") == 0)
				cfg.vi4 = Torneko3ParseU32(value, cfg.vi4);
			else if (StringUtil::Strcasecmp(key, "require_vi5") == 0)
				cfg.require_vi5 = Torneko3ParseBool(value, cfg.require_vi5);
			else if (StringUtil::Strcasecmp(key, "vi5") == 0)
				cfg.vi5 = Torneko3ParseU32(value, cfg.vi5);
			else if (StringUtil::Strcasecmp(key, "require_gif_tag") == 0)
				cfg.require_gif_tag = Torneko3ParseBool(value, cfg.require_gif_tag);
			else if (StringUtil::Strcasecmp(key, "gif_qword") == 0)
				cfg.gif_qword = Torneko3ParseU32(value, cfg.gif_qword);
			else if (StringUtil::Strcasecmp(key, "gif_nloop") == 0)
				cfg.gif_nloop = Torneko3ParseU32(value, cfg.gif_nloop);
			else if (StringUtil::Strcasecmp(key, "gif_flg") == 0)
				cfg.gif_flg = Torneko3ParseU32(value, cfg.gif_flg);
			else if (StringUtil::Strcasecmp(key, "gif_nreg") == 0)
				cfg.gif_nreg = Torneko3ParseU32(value, cfg.gif_nreg);
			else if (StringUtil::Strcasecmp(key, "gif_regs_mask") == 0)
				cfg.gif_regs_mask = Torneko3ParseU64(value, cfg.gif_regs_mask);
			else if (StringUtil::Strcasecmp(key, "gif_regs_value") == 0)
				cfg.gif_regs_value = Torneko3ParseU64(value, cfg.gif_regs_value);
			else if (StringUtil::Strcasecmp(key, "max_captures_per_pc") == 0)
				cfg.max_captures_per_pc = Torneko3ParseU32(value, cfg.max_captures_per_pc);
		}
		std::fclose(fp);
	}

	Torneko3MakeCaptureDir(cfg.dir);
	std::snprintf(cfg.ini_path, sizeof(cfg.ini_path), "%s", ini_path);
	{
		char loaded_path[512];
#ifdef _WIN32
		std::snprintf(loaded_path, sizeof(loaded_path), "%s\\torneko3_vu1_config_loaded.txt", cfg.dir);
#else
		std::snprintf(loaded_path, sizeof(loaded_path), "%s/torneko3_vu1_config_loaded.txt", cfg.dir);
#endif
		if (std::FILE* fp = std::fopen(loaded_path, "wb"))
		{
			std::fprintf(fp, "ini=%s\n", cfg.ini_path);
			std::fprintf(fp, "pcs=%u\n", cfg.pc_count);
			std::fprintf(fp, "require_q00a_xyz=%d\n", cfg.require_q00a_xyz ? 1 : 0);
			std::fprintf(fp, "require_vi4=%d\n", cfg.require_vi4 ? 1 : 0);
			std::fprintf(fp, "require_vi5=%d\n", cfg.require_vi5 ? 1 : 0);
			std::fprintf(fp, "require_gif_tag=%d\n", cfg.require_gif_tag ? 1 : 0);
			std::fclose(fp);
		}
	}
	return cfg;
}

static bool Torneko3PcEnabled(u32 pc)
{
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	for (u32 i = 0; i < cfg.pc_count; i++)
	{
		if (cfg.pcs[i] == pc)
			return true;
	}
	return false;
}

static bool Torneko3TargetSignatureMatches()
{
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	if (!cfg.require_q00a_xyz)
		return true;
	const u32* q00a = reinterpret_cast<const u32*>(&vuRegs[1].Mem[0x00a * 16]);
	return q00a[0] == cfg.q00a_xyz[0] && q00a[1] == cfg.q00a_xyz[1] && q00a[2] == cfg.q00a_xyz[2];
}

static bool Torneko3Strip0PacketSignatureMatches()
{
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	if (!cfg.require_gif_tag)
		return true;
	const u64* q087 = reinterpret_cast<const u64*>(&vuRegs[1].Mem[(cfg.gif_qword & 0x3ff) * 16]);
	const u32 nloop = static_cast<u32>(q087[0] & 0x7fff);
	const u32 flg = static_cast<u32>((q087[0] >> 58) & 0x3);
	const u32 nreg_raw = static_cast<u32>((q087[0] >> 60) & 0xf);
	const u64 regs = q087[1];
	return nloop == cfg.gif_nloop && flg == cfg.gif_flg && nreg_raw == cfg.gif_nreg && (regs & cfg.gif_regs_mask) == cfg.gif_regs_value;
}

static bool Torneko3TargetStateMatches(u32 pc)
{
	if (!Torneko3PcEnabled(pc) || !Torneko3TargetSignatureMatches() || !Torneko3Strip0PacketSignatureMatches())
		return false;

	const VURegs& r = vuRegs[1];
	const u32 vi4 = r.VI[4].UL & 0xffff;
	const u32 vi5 = r.VI[5].UL & 0xffff;
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	if (cfg.require_vi4 && vi4 != cfg.vi4)
		return false;
	return !cfg.require_vi5 || vi5 == cfg.vi5;
}

static void Torneko3TraceAttempt(u32 pc, const char* result)
{
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	char path[512];
#ifdef _WIN32
	std::snprintf(path, sizeof(path), "%s\\torneko3_vu1_trace.log", cfg.dir);
#else
	std::snprintf(path, sizeof(path), "%s/torneko3_vu1_trace.log", cfg.dir);
#endif
	if (std::FILE* fp = std::fopen(path, "ab"))
	{
		std::fprintf(fp, "pc=0x%04x result=%s vi4=0x%04x vi5=0x%04x\n", pc, result, vuRegs[1].VI[4].UL & 0xffff, vuRegs[1].VI[5].UL & 0xffff);
		std::fclose(fp);
	}
}

void Torneko3TraceCompileVU1State(u32 pc)
{
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	char path[512];
#ifdef _WIN32
	std::snprintf(path, sizeof(path), "%s\\torneko3_vu1_compile.log", cfg.dir);
#else
	std::snprintf(path, sizeof(path), "%s/torneko3_vu1_compile.log", cfg.dir);
#endif
	if (std::FILE* fp = std::fopen(path, "ab"))
	{
		std::fprintf(fp, "pc=0x%04x cfgpcs=%u req_q00a=%d req_vi4=%d req_vi5=%d req_gif=%d\n",
			pc, cfg.pc_count, cfg.require_q00a_xyz ? 1 : 0, cfg.require_vi4 ? 1 : 0, cfg.require_vi5 ? 1 : 0, cfg.require_gif_tag ? 1 : 0);
		std::fclose(fp);
	}
}

static u32& Torneko3CaptureCount(u32 pc)
{
	static u32 pc0138 = 0;
	static u32 pc0140 = 0;
	static u32 pc0170 = 0;
	static u32 pc0178 = 0;
	static u32 pc0180 = 0;
	static u32 pc0188 = 0;
	static u32 pc01e8 = 0;
	static u32 pc0230 = 0;
	static u32 pc0238 = 0;
	static u32 pc0248 = 0;
	static u32 pc01b8 = 0;
	static u32 pc01c0 = 0;
	static u32 pc01c8 = 0;
	static u32 pc01d0 = 0;
	static u32 pc0208 = 0;
	static u32 pc02a8 = 0;
	static u32 pc0250 = 0;
	static u32 pc0258 = 0;
	static u32 pc02b0 = 0;
	static u32 pc02b8 = 0;
	static u32 pc02c0 = 0;
	static u32 pc02c8 = 0;
	static u32 pc02d0 = 0;
	static u32 pc02d8 = 0;
	static u32 pc02e0 = 0;
	static u32 pc02e8 = 0;
	static u32 pc02f0 = 0;
	static u32 pc02f8 = 0;
	static u32 pc0340 = 0;
	static u32 pc0348 = 0;
	static u32 pc0350 = 0;
	static u32 pc03f0 = 0;
	static u32 pc0428 = 0;
	static u32 pc0430 = 0;
	static u32 pc0450 = 0;
	static u32 pc0458 = 0;
	static u32 pc0460 = 0;
	static u32 pc0468 = 0;
	static u32 pc0480 = 0;
	static u32 pc0488 = 0;
	static u32 pc0490 = 0;
	static u32 pc04e8 = 0;
	static u32 pc04f0 = 0;
	static u32 pc0818 = 0;
	static u32 pc0820 = 0;
	static u32 pc0838 = 0;
	static u32 pc0840 = 0;
	static u32 pc0a80 = 0;
	static u32 unknown = 0;

	switch (pc)
	{
		case 0x0138: return pc0138;
		case 0x0140: return pc0140;
		case 0x0170: return pc0170;
		case 0x0178: return pc0178;
		case 0x0180: return pc0180;
		case 0x0188: return pc0188;
		case 0x01e8: return pc01e8;
		case 0x0230: return pc0230;
		case 0x0238: return pc0238;
		case 0x0248: return pc0248;
		case 0x01b8: return pc01b8;
		case 0x01c0: return pc01c0;
		case 0x01c8: return pc01c8;
		case 0x01d0: return pc01d0;
		case 0x0208: return pc0208;
		case 0x02a8: return pc02a8;
		case 0x0250: return pc0250;
		case 0x0258: return pc0258;
		case 0x02b0: return pc02b0;
		case 0x02b8: return pc02b8;
		case 0x02c0: return pc02c0;
		case 0x02c8: return pc02c8;
		case 0x02d0: return pc02d0;
		case 0x02d8: return pc02d8;
		case 0x02e0: return pc02e0;
		case 0x02e8: return pc02e8;
		case 0x02f0: return pc02f0;
		case 0x02f8: return pc02f8;
		case 0x0340: return pc0340;
		case 0x0348: return pc0348;
		case 0x0350: return pc0350;
		case 0x03f0: return pc03f0;
		case 0x0428: return pc0428;
		case 0x0430: return pc0430;
		case 0x0450: return pc0450;
		case 0x0458: return pc0458;
		case 0x0460: return pc0460;
		case 0x0468: return pc0468;
		case 0x0480: return pc0480;
		case 0x0488: return pc0488;
		case 0x0490: return pc0490;
		case 0x04e8: return pc04e8;
		case 0x04f0: return pc04f0;
		case 0x0818: return pc0818;
		case 0x0820: return pc0820;
		case 0x0838: return pc0838;
		case 0x0840: return pc0840;
		case 0x0a80: return pc0a80;
		default: return unknown;
	}
}

static const char* Torneko3CaptureName(u32 pc)
{
	switch (pc)
	{
		case 0x0138: return "pc0138_vertex0";
		case 0x0140: return "pc0140_vertex0";
		case 0x0170: return "pc0170_vertex0";
		case 0x0178: return "pc0178_vertex0";
		case 0x0180: return "pc0180_vertex0";
		case 0x0188: return "pc0188_vertex0";
		case 0x01e8: return "pc01e8_vertex1";
		case 0x0230: return "pc0230_vertex1";
		case 0x0238: return "pc0238_vertex1";
		case 0x0248: return "pc0248_vertex1";
		case 0x01b8: return "pc01b8_vertex0";
		case 0x01c0: return "pc01c0_vertex0";
		case 0x01c8: return "pc01c8_vertex0";
		case 0x01d0: return "pc01d0_vertex0";
		case 0x0208: return "pc0208_vertex0";
		case 0x02a8: return "pc02a8_vertex1";
		case 0x0250: return "pc0250_vertex0";
		case 0x0258: return "pc0258_vertex0";
		case 0x02b0: return "pc02b0_vertex0";
		case 0x02b8: return "pc02b8_vertex0";
		case 0x02c0: return "pc02c0_vertex0";
		case 0x02c8: return "pc02c8_vertex0";
		case 0x02d0: return "pc02d0_vertex0";
		case 0x02d8: return "pc02d8_vertex0";
		case 0x02e0: return "pc02e0_vertex0";
		case 0x02e8: return "pc02e8_vertex0";
		case 0x02f0: return "pc02f0_vertex0";
		case 0x02f8: return "pc02f8_vertex0";
		case 0x0340: return "pc0340_vertex0";
		case 0x0348: return "pc0348_vertex0";
		case 0x0350: return "pc0350_vertex0";
		case 0x03f0: return "pc03f0_vertex1";
		case 0x0428: return "pc0428_vertex1";
		case 0x0430: return "pc0430_vertex1";
		case 0x0450: return "pc0450_vertex1";
		case 0x0458: return "pc0458_vertex1";
		case 0x0460: return "pc0460_vertex1";
		case 0x0468: return "pc0468_vertex1";
		case 0x0480: return "pc0480_vertex1";
		case 0x0488: return "pc0488_vertex1";
		case 0x0490: return "pc0490_vertex1";
		case 0x04e8: return "pc04e8_vertex1";
		case 0x04f0: return "pc04f0_vertex1";
		case 0x0818: return "xtop_pc0818";
		case 0x0820: return "xtop_pc0820";
		case 0x0838: return "target_pc0838";
		case 0x0840: return "target_pc0840";
		case 0x0a80: return "pc0a80_xgkick";
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

static bool Torneko3ReadEEMemory(u32 ee_addr, void* dst, u32 size)
{
	return vtlb_memSafeReadBytes(ee_addr, dst, size);
}

static void Torneko3WriteBinaryFile(const char* path, const void* data, size_t size)
{
	if (std::FILE* fp = std::fopen(path, "wb"))
	{
		std::fwrite(data, 1, size, fp);
		std::fclose(fp);
	}
}

static std::string Torneko3MakePath(const char* dir, const char* stem, const char* suffix)
{
	char path[768];
#ifdef _WIN32
	std::snprintf(path, sizeof(path), "%s\\%s%s", dir, stem, suffix);
#else
	std::snprintf(path, sizeof(path), "%s/%s%s", dir, stem, suffix);
#endif
	return std::string(path);
}

static u32 Torneko3ReadU32LE(const u8* p)
{
	return static_cast<u32>(p[0]) |
		(static_cast<u32>(p[1]) << 8) |
		(static_cast<u32>(p[2]) << 16) |
		(static_cast<u32>(p[3]) << 24);
}

static const char* Torneko3DmaTagName(u32 dma_id)
{
	switch (dma_id)
	{
		case 0: return "REFE";
		case 1: return "CNT";
		case 2: return "NEXT";
		case 3: return "REF";
		case 4: return "REFS";
		case 5: return "CALL";
		case 6: return "RET";
		case 7: return "END";
		default: return "UNKNOWN";
	}
}

static void Torneko3DumpCurrentDgDmaPayloads(const char* dir, const char* stem)
{
	constexpr u32 dg_dma_base = 0x00895980;
	constexpr u32 dg_dma_size = 0x00021a20;

	std::vector<u8> chain(dg_dma_size);
	if (!Torneko3ReadEEMemory(dg_dma_base, chain.data(), dg_dma_size))
		return;

	const std::string chain_path = Torneko3MakePath(dir, stem, "_ee_dgdma.bin");
	Torneko3WriteBinaryFile(chain_path.c_str(), chain.data(), chain.size());

	const std::string manifest_path = Torneko3MakePath(dir, stem, "_ee_dgdma_manifest.json");
	std::FILE* fp = std::fopen(manifest_path.c_str(), "wb");
	if (!fp)
		return;

	std::fprintf(fp, "{\n");
	std::fprintf(fp, "  \"dg_dma_base\": \"0x%08x\",\n", dg_dma_base);
	std::fprintf(fp, "  \"dg_dma_size\": \"0x%08x\",\n", dg_dma_size);
	std::fprintf(fp, "  \"chain_file\": \"%s_ee_dgdma.bin\",\n", stem);
	std::fprintf(fp, "  \"segments\": [\n");

	u32 offset = 0;
	bool first_segment = true;
	for (u32 index = 0; index < 512 && offset + 16 <= dg_dma_size; index++)
	{
		const u8* tag = &chain[offset];
		const u32 low = Torneko3ReadU32LE(tag + 0);
		const u32 addr = Torneko3ReadU32LE(tag + 4) & 0x7fffffff;
		const u32 upper0 = Torneko3ReadU32LE(tag + 8);
		const u32 upper1 = Torneko3ReadU32LE(tag + 12);
		const u32 qwc = low & 0xffff;
		const u32 dma_id = (low >> 28) & 0x7;
		const u32 payload_size = qwc * 16;
		const u32 next_offset = offset + 16;
		bool has_inline_payload = false;
		bool has_ref_payload = false;
		u32 payload_ee_addr = 0;
		std::string payload_file;

		if (dma_id == 1 || dma_id == 7)
		{
			has_inline_payload = (next_offset + payload_size <= dg_dma_size);
		}
		else if (dma_id == 0 || dma_id == 3 || dma_id == 4)
		{
			has_ref_payload = true;
			payload_ee_addr = addr;
			if (payload_size != 0)
			{
				std::vector<u8> payload(payload_size);
				if (Torneko3ReadEEMemory(payload_ee_addr, payload.data(), payload_size))
				{
					char suffix[128];
					std::snprintf(suffix, sizeof(suffix), "_seg%03u_ref_%08x_qwc%04x.bin", index, payload_ee_addr, qwc);
					payload_file = suffix;
					const std::string payload_path = Torneko3MakePath(dir, stem, suffix);
					Torneko3WriteBinaryFile(payload_path.c_str(), payload.data(), payload.size());
				}
			}
		}

		if (!first_segment)
			std::fprintf(fp, ",\n");
		first_segment = false;

		std::fprintf(fp, "    {\n");
		std::fprintf(fp, "      \"index\": %u,\n", index);
		std::fprintf(fp, "      \"tag_ee_addr\": \"0x%08x\",\n", dg_dma_base + offset);
		std::fprintf(fp, "      \"tag_offset\": \"0x%08x\",\n", offset);
		std::fprintf(fp, "      \"tag_low\": \"0x%08x\",\n", low);
		std::fprintf(fp, "      \"tag_addr\": \"0x%08x\",\n", addr);
		std::fprintf(fp, "      \"qwc\": %u,\n", qwc);
		std::fprintf(fp, "      \"dma_id\": %u,\n", dma_id);
		std::fprintf(fp, "      \"dma_name\": \"%s\",\n", Torneko3DmaTagName(dma_id));
		std::fprintf(fp, "      \"upper_vif_words\": [\"0x%08x\", \"0x%08x\"],\n", upper0, upper1);
		if (has_inline_payload)
			std::fprintf(fp, "      \"inline_payload_ee_addr\": \"0x%08x\",\n", dg_dma_base + next_offset);
		else
			std::fprintf(fp, "      \"inline_payload_ee_addr\": null,\n");
		if (has_ref_payload)
			std::fprintf(fp, "      \"ref_payload_ee_addr\": \"0x%08x\",\n", payload_ee_addr);
		else
			std::fprintf(fp, "      \"ref_payload_ee_addr\": null,\n");
		std::fprintf(fp, "      \"payload_size\": \"0x%08x\",\n", payload_size);
		if (!payload_file.empty())
			std::fprintf(fp, "      \"payload_file\": \"%s%s\",\n", stem, payload_file.c_str());
		else
			std::fprintf(fp, "      \"payload_file\": null,\n");
		std::fprintf(fp, "      \"payload_capture_ok\": %s\n", (!payload_file.empty() || (!has_ref_payload && !has_inline_payload)) ? "true" : "false");
		std::fprintf(fp, "    }");

		if (dma_id == 0 || dma_id == 7)
			break;

		if (dma_id == 1 || dma_id == 7 || dma_id == 2)
			offset = next_offset + payload_size;
		else
			offset = next_offset;

		if (offset > dg_dma_size)
			break;
	}

	std::fprintf(fp, "\n  ]\n");
	std::fprintf(fp, "}\n");
	std::fclose(fp);
}

void Torneko3DumpTargetVU1State(u32 pc)
{
	const Torneko3CaptureConfig& cfg = Torneko3Config();
	u32& capture_count = Torneko3CaptureCount(pc);
	if (capture_count >= cfg.max_captures_per_pc)
		return;
	if (!Torneko3TargetStateMatches(pc))
	{
		Torneko3TraceAttempt(pc, "filtered");
		return;
	}
	const u32 capture_index = capture_count++;
	Torneko3TraceAttempt(pc, "captured");

	const char* dir = cfg.dir;
	const char* name = Torneko3CaptureName(pc);
	char stem[512];
	char json_path[512];
	char mem_path[512];
#ifdef _WIN32
	std::snprintf(stem, sizeof(stem), "%s_%03u", name, capture_index);
	std::snprintf(json_path, sizeof(json_path), "%s\\%s_%03u_regs.json", dir, name, capture_index);
	std::snprintf(mem_path, sizeof(mem_path), "%s\\%s_%03u_vumem.bin", dir, name, capture_index);
#else
	std::snprintf(stem, sizeof(stem), "%s_%03u", name, capture_index);
	std::snprintf(json_path, sizeof(json_path), "%s/%s_%03u_regs.json", dir, name, capture_index);
	std::snprintf(mem_path, sizeof(mem_path), "%s/%s_%03u_vumem.bin", dir, name, capture_index);
#endif

	if (std::FILE* mem = std::fopen(mem_path, "wb"))
	{
		std::fwrite(vuRegs[1].Mem, 1, 0x4000, mem);
		std::fclose(mem);
	}

	if (pc == 0x0a80)
		Torneko3DumpCurrentDgDmaPayloads(dir, stem);

	std::FILE* fp = std::fopen(json_path, "wb");
	if (!fp)
		return;

	const VURegs& r = vuRegs[1];
	const microVU& m = microVU1;
	const microOp& op = m.prog.IRinfo.info[pc / 8];
	const u32* pq = &m.xmmBackup[xmmPQ.Id][0];
	std::fprintf(fp, "{\n");
	std::fprintf(fp, "  \"pc_before\": \"0x%04x\",\n", pc);
	std::fprintf(fp, "  \"capture_index\": %u,\n", capture_index);
	std::fprintf(fp, "  \"capture_stage\": \"strip0_runtime_transform\",\n");
	std::fprintf(fp, "  \"config\": {\"require_q00a_xyz\": %s, \"require_vi4\": %s, \"vi4\": \"0x%04x\", \"require_vi5\": %s, \"vi5\": \"0x%04x\", \"require_gif_tag\": %s, \"gif_qword\": \"0x%03x\", \"gif_nloop\": %u, \"gif_flg\": %u, \"gif_nreg\": %u, \"gif_regs_mask\": \"0x%llx\", \"gif_regs_value\": \"0x%llx\", \"max_captures_per_pc\": %u},\n",
		cfg.require_q00a_xyz ? "true" : "false", cfg.require_vi4 ? "true" : "false", cfg.vi4, cfg.require_vi5 ? "true" : "false", cfg.vi5,
		cfg.require_gif_tag ? "true" : "false", cfg.gif_qword, cfg.gif_nloop, cfg.gif_flg, cfg.gif_nreg,
		static_cast<unsigned long long>(cfg.gif_regs_mask), static_cast<unsigned long long>(cfg.gif_regs_value), cfg.max_captures_per_pc);
	std::fprintf(fp, "  \"target_signature\": {\"q00a_xyz_raw\": [\"0x%08x\", \"0x%08x\", \"0x%08x\"]},\n", cfg.q00a_xyz[0], cfg.q00a_xyz[1], cfg.q00a_xyz[2]);
	std::fprintf(fp, "  \"vu1_memory_file\": \"%s_%03u_vumem.bin\",\n", name, capture_index);
	std::fprintf(fp, "  \"qwords\": {\n");
	Torneko3WriteMemQwordJson(fp, "q0x008", 0x008); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x009", 0x009); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x00a", 0x00a); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x088", 0x088); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x089", 0x089); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x08a", 0x08a); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x08b", 0x08b); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x08c", 0x08c); std::fprintf(fp, ",\n");
	Torneko3WriteMemQwordJson(fp, "q0x08d", 0x08d); std::fprintf(fp, "\n");
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
	std::fprintf(fp, "    \"pending_p\": \"0x%08x\",\n", r.pending_p);
	std::fprintf(fp, "    \"mvu_q_instance\": %u,\n", m.q);
	std::fprintf(fp, "    \"mvu_p_instance\": %u,\n", m.p);
	std::fprintf(fp, "    \"mvu_read_q_instance\": %d,\n", op.readQ);
	std::fprintf(fp, "    \"mvu_write_q_instance\": %d,\n", op.writeQ);
	std::fprintf(fp, "    \"xmmpq_raw\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"],\n", pq[0], pq[1], pq[2], pq[3]);
	std::fprintf(fp, "    \"xmmpq_float\": [%.9g, %.9g, %.9g, %.9g],\n", Torneko3RawToFloat(pq[0]), Torneko3RawToFloat(pq[1]), Torneko3RawToFloat(pq[2]), Torneko3RawToFloat(pq[3]));
	std::fprintf(fp, "    \"active_q\": {\"raw\": \"0x%08x\", \"float\": %.9g},\n", r.q.UL, r.q.F);
	std::fprintf(fp, "    \"pending_q_value\": {\"raw\": \"0x%08x\", \"float\": %.9g},\n", r.pending_q, Torneko3RawToFloat(r.pending_q));
	std::fprintf(fp, "    \"active_p\": {\"raw\": \"0x%08x\", \"float\": %.9g},\n", r.p.UL, r.p.F);
	std::fprintf(fp, "    \"pending_p_value\": {\"raw\": \"0x%08x\", \"float\": %.9g}\n", r.pending_p, Torneko3RawToFloat(r.pending_p));
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
