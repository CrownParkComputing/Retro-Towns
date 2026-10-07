/* Retro-Towns core probe - the first N instructions, printed.
 *
 * This is how the Android black screen was found.  The default-fidelity
 * interpreter booted Turbo OutRun on x86-64 and aborted a few hundred
 * microseconds into the FM TOWNS IPL on aarch64, identically under -O0 and
 * -O3, with byte-identical ROM images.  Two runs that deterministic are not a
 * mystery, they are a diff.
 *
 * The diff put the divergence on one instruction - fc00:1787, a LOOP with a
 * displacement of 0xF1.  x86-64 went backwards 15 bytes, aarch64 forwards 241.
 * cpputil::GetSignedByte was sign-extending through a plain `char *`, and plain
 * char is signed on x86-64 and unsigned on ARM, so every negative imm8 in the
 * instruction set - every backward jump, every LOOP, every subs - became a
 * positive one on the device.
 *
 * The trace is printed from the loop's own counter rather than from the
 * debugger's CS:EIP ring: the ring has slots that were never written, so its
 * line numbers do not mean "instruction number" and a divergence found in it
 * cannot be pointed at.
 *
 * A trace alone says where two runs disagree, not why, so the tool can also
 * freeze-dry the machine around a given instruction: the registers, the flags,
 * and the bytes the CPU is about to fetch.  That turns "EIP differs by 0x100"
 * into an opcode anyone can read.
 *
 *   probe <rom-dir> <disc-image> [instructions] [dump-from] [dump-to]
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>
#include <vector>

#include "towns.h"
#include "townsargv.h"
#include "headless_mode.h"

/* Tsugaru narrates itself on std::cout - the ROM loader, the device that
 * aborted, the run-mode printer - and that narration is not the trace.  On a
 * desktop the two can be separated with a terminal; here the file has to be
 * comparable with `diff`, so the core's chatter is dropped and the one thing
 * worth keeping from it, the abort reason, is printed at the end. */
class NullBuf : public std::streambuf
{
protected:
	int overflow(int c) override { return 0!=(unsigned int)c?0:-1; }
	int sync(void) override { return 0; }
	std::streamsize xsputn(const char *,std::streamsize n) override { return n; }
};

/* The frame the machine is in, as text, plus the sixteen bytes at the linear
 * address the instruction fetch will read.  The CPU keeps the segment
 * descriptors itself, so the linear address is its state rather than an
 * assumption about how FMT_SYS.ROM is mapped. */
static void DumpState(FMTownsCommon &towns,unsigned int index)
{
	auto &cpu=towns.CPU();
	std::printf("--- instruction %u ---\n",index);
	for(auto &str : cpu.GetStateText())
	{
		std::printf("%s\n",str.c_str());
	}
	const auto linear=cpu.state.CS().baseLinearAddr+cpu.state.EIP;
	std::printf("BYTES %08x:",linear);
	for(unsigned int i=0; i<16; ++i)
	{
		std::printf(" %02x",cpu.DebugFetchByteByLinearAddress(towns.mem,linear+i)&0xffu);
	}
	std::printf("\n");
	std::fflush(stdout);
}

int main(int ac,char *av[])
{
	if(3>ac)
	{
		std::fprintf(stderr,"Usage: probe <rom-dir> <disc-image> [instructions] [dump-from] [dump-to]\n");
		return 2;
	}
	const unsigned int steps=(3<ac)?(unsigned int)std::atoi(av[3]):2000;
	const unsigned int dumpFrom=(4<ac)?(unsigned int)std::atoi(av[4]):0;
	const unsigned int dumpTo=(5<ac)?(unsigned int)std::atoi(av[5]):0;

	static NullBuf nullOut;
	std::cout.rdbuf(&nullOut);

	std::vector <std::string> argStr={"probe",av[1],"-CD",av[2]};
	std::vector <char*> argv;
	for(auto &s : argStr)
	{
		argv.push_back(s.data());
	}

	TownsARGV townsArgv;
	if(true!=townsArgv.AnalyzeCommandParameter(static_cast <int> (argv.size()),argv.data()))
	{
		std::fprintf(stderr,"Command line rejected.\n");
		return 2;
	}
	townsArgv.autoStart=true;

	/* Neither the machine nor the world it lives in belongs on a thread stack -
	 * both are far too big for one, and a tool that dies before it prints
	 * anything is worse than no tool at all. */
	auto world=std::make_unique <Headless_Mode> ();
	auto window=world->CreateWindowInterface();
	auto sound=world->CreateSound();

	auto towns=std::make_unique <FMTownsTemplate <i486DXDefaultFidelity> > ();
	if(true!=FMTownsCommon::Setup(*towns,world.get(),window,townsArgv))
	{
		std::fprintf(stderr,"Setup failed.\n");
		return 3;
	}

	/* Attaching a debugger turns an unhandled guest exception into an immediate
	 * abort instead of a run that continues on garbage, which for a trace is the
	 * better failure. */
	towns->CPU().AttachDebugger(&towns->debugger);

	unsigned int run=0;
	for(; run<steps && 0==towns->GetStopFlags(); ++run)
	{
		const auto &cpu=towns->CPU();
		std::printf("%u %04x:%08x\n",run,
		            cpu.state.CS().value&0xffffu,cpu.state.EIP);
		if(run>=dumpFrom && run<dumpTo)
		{
			DumpState(*towns,run);
		}
		towns->RunOneInstruction();
		towns->pic.ProcessIRQ(towns->CPU(),towns->mem);
		towns->RunFastDevicePolling();
		towns->RunScheduledTasks();
		/* The state the window ends on is the one the other platform has to
		 * match, so it is worth as much as the instructions inside it. */
		if(run+1==dumpTo)
		{
			DumpState(*towns,run+1);
		}
	}

	std::fflush(stdout);
	std::fprintf(stderr,"%u executed, stop=0x%02x, townsTime=%lld, abort=%s (%s)\n",
	             run,(unsigned int)towns->GetStopFlags(),
	             (long long)towns->state.townsTime,
	             towns->vmAbortReason.c_str(),towns->vmAbortDeviceName.c_str());

	world->DeleteWindowInterface(window);
	world->DeleteSound(sound);
	return 0;
}
