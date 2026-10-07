/* Retro-Towns - implementation of the C ABI in ftowns_bridge.h.
 *
 * This file is the only place the front end touches Tsugaru, so it is the only
 * place that knows Tsugaru has threads, templates and a command interpreter.
 * It is deliberately modelled on src/main_headless, which upstream already
 * ships as the windowless reference host: same VM thread, same UI thread, same
 * command queue, with the two places headless returns nothing - the frame and
 * the sound - filled in for real.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftowns_bridge.h"

#include "towns.h"
#include "townsdef.h"
#include "townsthread.h"
#include "townscommand.h"
#include "townsargv.h"
#include "discimg.h"
#include "outside_world.h"
#include "render.h"

namespace {

/* How long a queued audio stretch may sit before the core is told the channel
 * is free.  The core paces itself on FMPCMChannelPlaying(), which is how a
 * 40 ms-at-a-time synthesiser is kept in step with an output device; if the
 * host stopped pulling, an honest "still playing" would wedge the VM forever.
 */
constexpr int64_t STALE_AUDIO_MS=500;

constexpr uint32_t MIX_RATE=FTOWNS_AUDIO_RATE;

/* How much audio the core is asked to keep in flight, in frames.
 *
 * FMPCMChannelPlaying() is not a yes/no question about the past - it is the
 * core's throttle.  The synthesiser makes one wave at a time and will only make
 * the next one once this says "free", so whatever it answers sets how deep the
 * queue is.  Answering "free" only when the queue is literally empty costs a
 * wave every chunk boundary: the queue is dry until the core notices and
 * regenerates, and at 40 ms per wave that is an audible buzz, not a rounding
 * error.
 *
 * Upstream's ALSA backend answers "room for one more" - it holds a playing slot
 * and a stand-by slot, so the queue sits at two waves and the drain is
 * continuous.  That is the depth copied here rather than a deeper one, because
 * every wave is stamped with the emulated time it was made at
 * (lastFMPCMWaveGenTime): letting the core run further ahead than the device
 * can consume would advance the FM envelopes as if a whole frame had passed
 * when it had not.
 */
constexpr size_t AUDIO_KEEP_FRAMES=
    static_cast <size_t> (TownsSound::FM_PCM_MILLISEC_PER_WAVE)*MIX_RATE/1000;  /* One wave. */

/* Diagnostic: the raw bytes a channel was handed, straight to a file.
 *
 * "The sound is wrong" has two very different causes - the core synthesised the
 * wrong samples, or the host mixed them wrongly - and they want opposite fixes.
 * Catching the wave before it joins the mix tells them apart without anyone
 * having to guess by ear.  Raw headerless s16le stereo, 44100 Hz.
 */
class ChannelDump
{
public:
	explicit ChannelDump(const char *path)
	{
		if(nullptr!=path && '\0'!=path[0])
		{
			file=fopen(path,"wb");
		}
	}
	~ChannelDump(void)
	{
		if(nullptr!=file)
		{
			fclose(file);
		}
	}
	ChannelDump(const ChannelDump &)=delete;
	ChannelDump &operator=(const ChannelDump &)=delete;

	void write(const unsigned char *p,size_t n)
	{
		if(nullptr!=file)
		{
			fwrite(p,1,n,file);
		}
	}

private:
	FILE *file=nullptr;
};

ChannelDump gFmDump(getenv("FTOWNS_DUMP_FM"));
ChannelDump gBeepDump(getenv("FTOWNS_DUMP_BEEP"));

/* One signed-16 sample added into a shared output slot.
 *
 * Every channel accumulates, so the order the channels are mixed in does not
 * matter and no channel has to know it is not the first.  The output buffer
 * starts zeroed, which is also what produces silence.
 */
static inline void add_saturate(int16_t &dst,int32_t add)
{
	const int32_t sum=static_cast <int32_t> (dst)+add;
	if(32767<sum)
	{
		dst=32767;
	}
	else if(-32768>sum)
	{
		dst=-32768;
	}
	else
	{
		dst=static_cast <int16_t> (sum);
	}
}

/* One 44100 Hz signed-16 stereo stretch waiting to be heard. */
struct WaveChunk
{
	std::vector <int16_t> sample;
	size_t pos=0;           /* in frames, not samples                     */

	size_t remain(void) const
	{
		return sample.size()/2-pos;
	}
};

/* A channel the core hands whole chunks to: FM/PCM and the buzzer.
 *
 * The core generates 44100 Hz for every channel it has - YM2612 gives
 * WAVE_SAMPLING_RATE, the timer gives BUZZER_SAMPLING_RATE - so there is no
 * resampling to do here and none should be added on speculation.
 */
class ChunkChannel
{
public:
	/* How much this channel had to throw away, and the deepest the queue ever
	 * got.  Throwing away audio is the sound of a note that never plays, so a
	 * non-zero `dropped` means the queue is the wrong shape rather than merely
	 * the wrong size. */
	long long dropped=0;
	size_t peak_queued=0;

	void push(const int16_t *sample,size_t frames)
	{
		WaveChunk chunk;
		chunk.sample.assign(sample,sample+frames*2);
		queued+=frames;
		chunks.push_back(std::move(chunk));
		if(peak_queued<queued)
		{
			peak_queued=queued;
		}
		while(queued>MAX_QUEUED_FRAMES && 1<chunks.size())
		{
			dropped+=chunks.front().remain();
			queued-=chunks.front().remain();
			chunks.pop_front();
		}
		/* A chunk longer than the whole limit is the core's own padding: pausing
		 * asks for the buzzer equivalent of two hundred seconds of silence.  It
		 * would otherwise sit at the head of the queue for its full length and
		 * delay every real tone behind it, so the oldest part goes.  Not counted
		 * as dropped, because `dropped` is the sound of a note that never
		 * played, and silence padding was never a note. */
		if(MAX_QUEUED_FRAMES<chunks.back().remain())
		{
			const auto skip=chunks.back().remain()-MAX_QUEUED_FRAMES;
			chunks.back().pos+=skip;
			queued-=skip;
		}
	}

	/* Fill `frames` into out[] honouring [vol].  Returns frames written.
	 *
	 * `vol` is signed on purpose.  An unsigned scale factor silently converts the
	 * whole expression to unsigned int, so `l*vol` wraps when l is negative and
	 * the unsigned divide then yields a huge positive addend - every negative
	 * sample saturates to full scale, the output loses its bottom half, and the
	 * result still sounds like "loud noise" rather than obviously broken.
	 */
	size_t mix(int16_t *out,size_t frames,int32_t vol)
	{
		size_t done=0;
		while(done<frames)
		{
			if(chunks.empty())
			{
				break;
			}
			auto &chunk=chunks.front();
			auto take=chunk.remain();
			if(frames-done<take)
			{
				take=frames-done;
			}
			for(size_t i=0; i<take; ++i)
			{
				const auto l=chunk.sample[(chunk.pos+i)*2];
				const auto r=chunk.sample[(chunk.pos+i)*2+1];
				const auto o=(done+i)*2;
				add_saturate(out[o],(static_cast <int32_t> (l)*vol)/256);
				add_saturate(out[o+1],(static_cast <int32_t> (r)*vol)/256);
			}
			chunk.pos+=take;
			done+=take;
			queued-=take;
			if(0==chunk.remain())
			{
				chunks.pop_front();
			}
		}
		return done;
	}

	size_t pending(void) const
	{
		return queued;
	}

	void clear(void)
	{
		chunks.clear();
		queued=0;
	}

private:
	static constexpr size_t MAX_QUEUED_FRAMES=MIX_RATE/2;  /* 500 ms */

	std::deque <WaveChunk> chunks;
	size_t queued=0;
};

/* CD-DA is not chunked - the core hands over a whole track and expects to be
 * told where the play head is - so it keeps its own buffer and cursor.
 */
class StreamChannel
{
public:
	void play(std::vector <int16_t> wave,unsigned int startHSG,bool repeat)
	{
		buffer=std::move(wave);
		pos=0;
		loopStartHSG=startHSG;
		repeating=repeat;
		playing=true;
		paused=false;
	}

	void stop(void)
	{
		playing=false;
		paused=false;
	}

	void pause(void)
	{
		paused=true;
	}

	void resume(void)
	{
		paused=false;
	}

	bool is_playing(void) const
	{
		return playing && !paused;
	}

	size_t mix(int16_t *out,size_t frames,int32_t volL,int32_t volR)
	{
		if(!playing || paused || buffer.empty())
		{
			return 0;
		}
		const size_t total=buffer.size()/2;
		size_t done=0;
		while(done<frames && pos<total)
		{
			const auto from=pos*2;
			const auto to=done*2;
			add_saturate(out[to],(static_cast <int32_t> (buffer[from])*volL)/256);
			add_saturate(out[to+1],(static_cast <int32_t> (buffer[from+1])*volR)/256);
			++done;
			++pos;
		}
		if(total<=pos)
		{
			if(repeating)
			{
				pos=0;
			}
			else
			{
				playing=false;
			}
		}
		return done;
	}

	/* Seconds elapsed within the current play request. */
	double position_sec(void) const
	{
		return static_cast <double> (pos)/static_cast <double> (MIX_RATE);
	}
	unsigned int start_hsg(void) const
	{
		return loopStartHSG;
	}
	void clear(void)
	{
		buffer.clear();
		buffer.shrink_to_fit();
		pos=0;
		playing=false;
		paused=false;
	}

private:
	std::vector <int16_t> buffer;
	size_t pos=0;
	unsigned int loopStartHSG=0;
	bool playing=false;
	bool paused=false;
	bool repeating=false;
};

/* Everything the audio callback touches, behind one lock.  The critical
 * section is a memcpy-scale mix of at most a few milliseconds, and the VM
 * thread only ever holds it to append a chunk, which is how upstream's
 * YsSoundPlayer worked too.
 */
struct Mixer
{
	std::mutex lock;
	ChunkChannel fmpcm;
	ChunkChannel beep;
	StreamChannel cdda;
	int32_t cddaVolL=256,cddaVolR=256;
	std::atomic <int64_t> lastPullMs{0};
	/* Frames asked for, and frames the FM/PCM channel actually had.  The
	 * channel is fed by a synthesiser that runs whether or not a tone is
	 * playing - the core pushes silence otherwise - so anything short of the
	 * request is the queue running dry, which is what makes the output buzz.
	 * Both counters are under `lock`. */
	long long pulled=0,covered=0;
};

Mixer gMixer;

int64_t now_ms(void)
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* ---------------------------------------------------------------- */
/* The window: frames out, level state in                            */
/* ---------------------------------------------------------------- */

class BridgeWorld;

class BridgeWindow : public Outside_World::WindowInterface
{
public:
	/* Published frame, owned here and read by ftowns_poll_frame. */
	std::mutex frameLock;
	unsigned int frameWid=0,frameHei=0;
	std::vector <unsigned char> frameRGBA;
	uint64_t frameSeq=0;
	uint64_t readSeq=0;

	void publish(const TownsRender::ImageCopy &img)
	{
		if(0==img.wid || 0==img.hei)
		{
			return;
		}
		std::lock_guard <std::mutex> lock(frameLock);
		frameWid=img.wid;
		frameHei=img.hei;
		frameRGBA=img.rgba;
		++frameSeq;
	}

	void Start(void) override {}
	void Stop(void) override {}

	/* Called from the window thread - which is the front end's main loop. */
	void Interval(void) override
	{
		BaseInterval();
		if(true==winThr.newImageRendered)
		{
			publish(winThr.mostRecentImage);
		}
	}

	void Render(bool) override
	{
		/* BaseInterval already rasterised and published; there is nothing to
		 * present here because this window does not own a surface. */
	}

	void UpdateImage(TownsRender::ImageCopy &img) override
	{
		publish(img);
	}

	void Communicate(Outside_World *world) override
	{
		std::lock_guard <std::mutex> lock(deviceStateLock);
		shared.showMouseCursor=world->showMouseCursor;
	}
};

/* ---------------------------------------------------------------- */
/* The sound: the core pushes, the host pulls                        */
/* ---------------------------------------------------------------- */

class BridgeSound : public Outside_World::Sound
{
public:
	void Start(void) override
	{
		gMixer.lastPullMs.store(now_ms());
	}
	void Stop(void) override {}

	void Polling(void) override {}

	void CDDAPlay(const DiscImage &discImg,DiscImage::MinSecFrm from,DiscImage::MinSecFrm to,bool repeat,unsigned int,unsigned int) override
	{
		auto wave=discImg.GetWave(from,to);
		std::vector <int16_t> s16(wave.size()/2);
		std::memcpy(s16.data(),wave.data(),wave.size());
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.cdda.play(std::move(s16),from.ToHSG(),repeat);
	}
	void CDDASetVolume(float leftVol,float rightVol) override
	{
		auto l=static_cast <int32_t> (256.0*leftVol);
		auto r=static_cast <int32_t> (256.0*rightVol);
		/* Clamped at both ends: a volume far above 1.0 only asks for clipping,
		 * and a negative one would invert a whole channel.  32768 also keeps
		 * sample*volume inside int32_t for a full-scale sample. */
		l=l<0?0:(32768<l?32768:l);
		r=r<0?0:(32768<r?32768:r);
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.cddaVolL=l;
		gMixer.cddaVolR=r;
	}
	void CDDAStop(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.cdda.stop();
	}
	void CDDAPause(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.cdda.pause();
	}
	void CDDAResume(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.cdda.resume();
	}
	bool CDDAIsPlaying(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		return gMixer.cdda.is_playing();
	}
	DiscImage::MinSecFrm CDDACurrentPosition(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		const auto hsg=gMixer.cdda.start_hsg()+static_cast <unsigned int> (gMixer.cdda.position_sec()*75.0);
		DiscImage::MinSecFrm msf;
		msf.FromHSG(hsg);
		return msf;
	}

	void FMPCMPlay(std::vector <unsigned char> &wave) override
	{
		gFmDump.write(wave.data(),wave.size());
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.fmpcm.push(reinterpret_cast <const int16_t*> (wave.data()),wave.size()/4);
	}
	void FMPCMPlayStop(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.fmpcm.clear();
	}
	bool FMPCMChannelPlaying(void) override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		if(STALE_AUDIO_MS<now_ms()-gMixer.lastPullMs.load())
		{
			/* Nobody is listening - do not hold the core's sound generator
			 * waiting for a host that is not consuming. */
			gMixer.fmpcm.clear();
			return false;
		}
		return AUDIO_KEEP_FRAMES<gMixer.fmpcm.pending();
	}

	void BeepPlay(int samplingRate,std::vector <unsigned char> &wave) override
	{
		(void)samplingRate;  /* always 44100 - see TownsTimer::BUZZER_SAMPLING_RATE */
		gBeepDump.write(wave.data(),wave.size());
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.beep.push(reinterpret_cast <const int16_t*> (wave.data()),wave.size()/4);
	}
	void BeepPlayStop() override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		gMixer.beep.clear();
	}
	bool BeepChannelPlaying() const override
	{
		std::lock_guard <std::mutex> lock(gMixer.lock);
		return AUDIO_KEEP_FRAMES<gMixer.beep.pending();
	}
};

/* ---------------------------------------------------------------- */
/* Outside_World: the host seam                                      */
/* ---------------------------------------------------------------- */

class BridgeWorld : public Outside_World
{
public:
	/* Input mirror, written by the front end, read on the VM thread. */
	std::mutex inputLock;
	ftowns_input input;
	struct KeyEvent
	{
		int jisKey;
		bool down;

		/* The TOWNS keyboard reports a shifted key as the same JIS number with
		 * a shift flag on the first byte - which is how the core's own
		 * ASCII-to-key translator marks `:` against `;` - so the flag has to
		 * travel with the event rather than be folded into the key number. */
		bool shift;
	};
	std::deque <KeyEvent> keyEvents;

	std::mutex cmdLock;
	std::deque <std::string> commands;

	BridgeWorld(void)
	{
		/* Hardware defaults, and the two ports Tsugaru's own parameters use:
		 * a pad in 0, a mouse in 1. */
		gamePort[0]=TOWNS_GAMEPORTEMU_PHYSICAL0;
		gamePort[1]=TOWNS_GAMEPORTEMU_MOUSE;
		lastMx=lastMy=0;
		SetKeyboardMode(TOWNS_KEYBOARD_MODE_DIRECT);
	}

	std::string GetProgramResourceDirectory(void) const override
	{
		return ".";
	}

	void Start(void) override {}
	void Stop(void) override {}

	bool ImageNeedsFlip(void) override
	{
		/* TownsRender writes row 0 at the top, which is what an SDL texture
		 * wants, so nothing is flipped anywhere - same answer
		 * FsSimpleWindowConnection gives on every platform. */
		return false;
	}

	void SetKeyboardLayout(unsigned int) override {}

	/* Runs on the VM thread, once per main-loop iteration. */
	void DevicePolling(FMTownsCommon &towns) override
	{
		ftowns_input in;
		{
			std::lock_guard <std::mutex> lock(inputLock);
			in=input;
		}

		for(int port=0; port<2; ++port)
		{
			switch(gamePort[port])
			{
			case TOWNS_GAMEPORTEMU_PHYSICAL0:
			case TOWNS_GAMEPORTEMU_PHYSICAL1:
			case TOWNS_GAMEPORTEMU_ANALOG0:
			case TOWNS_GAMEPORTEMU_ANALOG1:
				{
					const auto b=in.pad[port];
					towns.SetGamePadState(
					    port,
					    0!=(b&FTOWNS_PAD_A),0!=(b&FTOWNS_PAD_B),
					    0!=(b&FTOWNS_PAD_LEFT),0!=(b&FTOWNS_PAD_RIGHT),
					    0!=(b&FTOWNS_PAD_UP),0!=(b&FTOWNS_PAD_DOWN),
					    0!=(b&FTOWNS_PAD_RUN),0!=(b&FTOWNS_PAD_PAUSE),0!=(b&FTOWNS_PAD_ZOOM));
				}
				break;
			case TOWNS_GAMEPORTEMU_CYBERSTICK:
				towns.SetCyberStickState(
				    port,in.stick[port][0],in.stick[port][1],
				    in.stick[port][2],in.stick[port][3],in.trigger);
				break;
			case TOWNS_GAMEPORTEMU_MOUSE:
				{
					const auto lb=0!=(in.mouseButton&1u);
					const auto rb=0!=(in.mouseButton&2u);
					const auto mb=0!=(in.mouseButton&4u);
					this->ProcessMouse(towns,lb?1:0,mb?1:0,rb?1:0,in.mouseX,in.mouseY);
					if(0!=in.mouseRelX || 0!=in.mouseRelY)
					{
						towns.SetMouseMotion(port,in.mouseRelX,in.mouseRelY);
					}
				}
				break;
			default:
				break;
			}
		}

		/* Keys are make/break events, so they are drained rather than
		 * mirrored - and ProcessInkey is what runs the per-title
		 * augmentation the core does for things like Strike Commander. */
		for(;;)
		{
			KeyEvent ev;
			{
				std::lock_guard <std::mutex> lock(inputLock);
				if(keyEvents.empty())
				{
					break;
				}
				ev=keyEvents.front();
				keyEvents.pop_front();
			}
			towns.keyboard.PushFifo(
			    (ev.down?TOWNS_KEYFLAG_JIS_PRESS:TOWNS_KEYFLAG_JIS_RELEASE)|
			    (ev.shift?TOWNS_KEYFLAG_SHIFT:0),
			    static_cast <unsigned char> (ev.jisKey));
			ProcessInkey(towns,ev.jisKey);
		}
	}

	WindowInterface *CreateWindowInterface(void) const override
	{
		return new BridgeWindow;
	}
	void DeleteWindowInterface(WindowInterface *itfc) const override
	{
		delete itfc;
	}

	Sound *CreateSound(void) const override
	{
		return new BridgeSound;
	}
	void DeleteSound(Sound *itfc) const override
	{
		delete itfc;
	}
};

/* ---------------------------------------------------------------- */
/* The UI thread: feeds commands, owns nothing else                  */
/* ---------------------------------------------------------------- */

class BridgeUIThread : public TownsUIThread
{
public:
	BridgeWorld *world=nullptr;
	bool terminate=false;

	/* Runs on its own thread for the life of the VM. */
	void Main(TownsThread &vmThread,FMTownsCommon &towns,const TownsARGV &argv,Outside_World &outside_world) override
	{
		(void)vmThread;
		(void)towns;
		(void)argv;
		while(true!=terminate && true!=vmTerminated)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			for(;;)
			{
				std::string cmd;
				{
					std::lock_guard <std::mutex> lock(world->cmdLock);
					if(world->commands.empty())
					{
						break;
					}
					cmd=world->commands.front();
					world->commands.pop_front();
				}
				/* Handing it to the core's own queue is what makes !LOAD,
				 * !TYPE and the rest work without a private reimplementation
				 * of each - the VM thread pops them in ExecCommandQueue. */
				outside_world.commandQueue.push(cmd);
			}
		}
	}

	/* Runs on the VM thread, with uiLock held. */
	void ExecCommandQueue(TownsThread &vmThread,FMTownsCommon &towns,Outside_World *outside_world,Outside_World::Sound *sound) override
	{
		while(true!=outside_world->commandQueue.empty())
		{
			auto cmd=interpreter.Interpret(outside_world->commandQueue.front());
			outside_world->commandQueue.pop();
			interpreter.Execute(vmThread,towns,outside_world,sound,cmd);
			if(TownsCommandInterpreter::CMD_QUIT==cmd.primaryCmd)
			{
				terminate=true;
			}
		}
	}

	TownsCommandInterpreter interpreter;
};

/* ---------------------------------------------------------------- */
/* Bridge state                                                      */
/* ---------------------------------------------------------------- */

struct Session
{
	BridgeWorld world;
	BridgeWorld::WindowInterface *window=nullptr;
	Outside_World::Sound *sound=nullptr;
	BridgeUIThread uiThread;
	TownsARGV argv;
	TownsThread vmThread;

	/* Exactly one of these is alive; which one depends on the fidelity the
	 * command line asked for, and VMMainLoop is templated on it. */
	FMTownsTemplate <i486DXDefaultFidelity> *townsDefault=nullptr;
	FMTownsTemplate <i486DXHighFidelity> *townsHigh=nullptr;

	std::thread vmWorker;
	std::thread uiWorker;

	std::atomic <bool> active{false};
	std::atomic <int> requestedMode{-1};

	BridgeWindow *bridge_window(void)
	{
		return static_cast <BridgeWindow*> (window);
	}
};

Session *gSession=nullptr;

ftowns_log_fn gLogFn=nullptr;
void *gLogCtx=nullptr;

void log_line(const char *line)
{
	if(nullptr!=gLogFn)
	{
		gLogFn(line,gLogCtx);
	}
}

/* Tsugaru narrates itself on std::cout: the ROM loader, the device that
 * aborts the machine, the run-mode printer.  A desktop host has a terminal to
 * put that in; an APK has none, so the port was throwing away its best
 * diagnostic.  Teeing it into the log callback costs a copy per line and still
 * shows the text wherever the process writes. */
class LogTee : public std::streambuf
{
public:
	explicit LogTee(std::streambuf *downIn): down(downIn) {}
	~LogTee() override { Emit(); }

protected:
	std::streamsize xsputn(const char *s,std::streamsize n) override
	{
		for(std::streamsize i=0; i<n; ++i)
		{
			if('\n'==s[i])
			{
				Emit();
			}
			else
			{
				line+=s[i];
			}
		}
		if(nullptr!=down)
		{
			down->sputn(s,n);
		}
		return n;
	}
	int overflow(int c) override
	{
		if(std::char_traits<char>::eof()==c)
		{
			return 0;
		}
		const char ch=(char)c;
		xsputn(&ch,1);
		return c;
	}
	int sync(void) override
	{
		Emit();
		return 0;
	}

private:
	void Emit(void)
	{
		if(false==line.empty())
		{
			log_line(line.c_str());
			line.clear();
		}
	}
	std::string line;
	std::streambuf *down;
};

/* Installed once, for the life of the process - the streams go to the log as
 * well as wherever the host already sends them. */
void tee_console_to_log(void)
{
	static LogTee coutTee(std::cout.rdbuf());
	static LogTee cerrTee(std::cerr.rdbuf());
	static bool installed=false;
	if(false==installed)
	{
		std::cout.rdbuf(&coutTee);
		std::cerr.rdbuf(&cerrTee);
		installed=true;
	}
}

void run_vm(void)
{
	auto &s=*gSession;
	if(nullptr!=s.townsHigh)
	{
		s.vmThread.VMStart(s.townsHigh,&s.world,&s.uiThread);
		s.vmThread.VMMainLoop(s.townsHigh,&s.world,s.sound,s.window,&s.uiThread);
		s.vmThread.VMEnd(s.townsHigh,&s.world,&s.uiThread);
	}
	else
	{
		s.vmThread.VMStart(s.townsDefault,&s.world,&s.uiThread);
		s.vmThread.VMMainLoop(s.townsDefault,&s.world,s.sound,s.window,&s.uiThread);
		s.vmThread.VMEnd(s.townsDefault,&s.world,&s.uiThread);
	}
}

}  /* namespace */

/* ================================================================== */
/* C ABI                                                              */
/* ================================================================== */

extern "C" int ftowns_start(int argc,const char *const *argv)
{
	tee_console_to_log();

	if(nullptr!=gSession)
	{
		return FTOWNS_ERR_ALREADY_RUNNING;
	}
	if(nullptr==argv || nullptr==argv[0])
	{
		return FTOWNS_ERR_BAD_ARG;
	}
	/* argv[1] is the ROM directory.  Without it the core falls back to
	 * getcwd(), which on Android is not anywhere the ROMs live - so the
	 * front end always passes it and nothing has to be patched upstream. */
	if(argc<2 || nullptr==argv[1])
	{
		log_line("ftowns_start: no ROM directory (argv[1])");
		return FTOWNS_ERR_BAD_ARG;
	}

	auto *townsArgv=const_cast <char**> (argv);
	auto *session=new Session;
	session->uiThread.world=&session->world;

	if(true!=session->argv.AnalyzeCommandParameter(argc,townsArgv))
	{
		log_line("ftowns_start: command line rejected");
		delete session;
		return FTOWNS_ERR_BAD_ARG;
	}
	session->argv.autoStart=true;

	Outside_World *world=&session->world;
	session->sound=world->CreateSound();
	session->window=world->CreateWindowInterface();

	const bool highFidelity=(i486DXCommon::HIGH_FIDELITY==session->argv.CPUFidelityLevel);
	bool setupOk=false;
	if(highFidelity)
	{
		session->townsHigh=new FMTownsTemplate <i486DXHighFidelity>;
		setupOk=FMTownsCommon::Setup(*session->townsHigh,world,session->window,session->argv);
	}
	else
	{
		session->townsDefault=new FMTownsTemplate <i486DXDefaultFidelity>;
		setupOk=FMTownsCommon::Setup(*session->townsDefault,world,session->window,session->argv);
	}
	if(true!=setupOk)
	{
		log_line("ftowns_start: Setup failed - ROM images or disc image could not be loaded");
		delete session->townsHigh;
		delete session->townsDefault;
		world->DeleteSound(session->sound);
		world->DeleteWindowInterface(session->window);
		delete session;
		return FTOWNS_ERR_SETUP;
	}

	session->window->Start();

	/* argv.autoStart is only a request - the host is the one that puts the
	 * machine in motion.  Without this the VM sits in its default
	 * RUNMODE_PAUSE, renders an IPL screen, and generates no sound at all. */
	session->vmThread.SetRunMode(TownsThread::RUNMODE_RUN);

	session->active.store(true);
	gSession=session;

	session->vmWorker=std::thread(run_vm);
	session->uiWorker=std::thread([]{
		gSession->uiThread.Run(&gSession->vmThread,gSession->townsDefault?static_cast<FMTownsCommon*>(gSession->townsDefault):static_cast<FMTownsCommon*>(gSession->townsHigh),&gSession->argv,&gSession->world);
	});

	return FTOWNS_OK;
}

extern "C" void ftowns_stop(void)
{
	auto *session=gSession;
	if(nullptr==session)
	{
		return;
	}
	{
		std::lock_guard <std::mutex> lock(session->world.cmdLock);
		session->world.commands.push_back("QUIT");
	}
	/* The VM thread has to unwind through VMEnd to write CMOS and close the
	 * disc images, so waiting is the whole shutdown - there is no shortcut
	 * that leaves the machine in a state worth saving. */
	session->uiWorker.join();
	session->vmWorker.join();

	session->world.DeleteWindowInterface(session->window);
	session->world.DeleteSound(session->sound);
	delete session->townsHigh;
	delete session->townsDefault;

	gSession=nullptr;
	delete session;
}

extern "C" int ftowns_is_running(void)
{
	return (nullptr!=gSession && true==gSession->active.load())?1:0;
}

extern "C" int ftowns_vm_state(void)
{
	if(nullptr==gSession)
	{
		return FTOWNS_VM_IDLE;
	}
	switch(gSession->vmThread.GetRunMode())
	{
	case TownsThread::RUNMODE_POWER_OFF:
		return FTOWNS_VM_POWER_OFF;
	case TownsThread::RUNMODE_PAUSE:
		return FTOWNS_VM_PAUSE;
	case TownsThread::RUNMODE_RUN:
	case TownsThread::RUNMODE_ONE_INSTRUCTION:
		return FTOWNS_VM_RUN;
	case TownsThread::RUNMODE_EXIT:
		return FTOWNS_VM_EXIT;
	default:
		return FTOWNS_VM_IDLE;
	}
}

extern "C" unsigned int ftowns_debug_info(char *buf,unsigned int cap)
{
	if(nullptr==buf || 0==cap)
	{
		return 0;
	}
	buf[0]='\0';
	if(nullptr==gSession)
	{
		return 0;
	}
	FMTownsCommon *towns=(nullptr!=gSession->townsHigh)
	    ? static_cast <FMTownsCommon*> (gSession->townsHigh)
	    : static_cast <FMTownsCommon*> (gSession->townsDefault);
	if(nullptr==towns)
	{
		return (unsigned int)std::snprintf(buf,cap,"no machine");
	}
	/* A machine that stops by itself says so only through these: a stop flag
	 * set by the device that gave up, an abort string naming it, or the
	 * power-off latch.  Without them the front end can see "paused" and has no
	 * way of knowing who did it. */
	const auto flags=towns->GetStopFlags();
	const char *dev=false==towns->vmAbortDeviceName.empty()
	    ? towns->vmAbortDeviceName.c_str() : "-";
	const char *why=false==towns->vmAbortReason.empty()
	    ? towns->vmAbortReason.c_str() : "-";
	return (unsigned int)std::snprintf(buf,cap,
	    "stop=0x%02x abort=%s (%s)  power=%s%s",
	    (unsigned int)flags,dev,why,
	    (true==towns->var.powerOff)?"off":"on",
	    (true==towns->var.pauseOnPowerOff)?" pauseOnPowerOff":"");
}

extern "C" void ftowns_set_run_mode(int mode)
{
	if(nullptr==gSession)
	{
		return;
	}
	/* SetRunMode mutates the loop variable of a running thread; the command
	 * interpreter is the one path that is allowed to do it, and it runs on
	 * the VM thread.  The queue takes bare command words - the leading '!'
	 * is a CUI input convention, not part of the command. */
	if(FTOWNS_VM_RUN==mode)
	{
		ftowns_command("RUN");
	}
	else if(FTOWNS_VM_PAUSE==mode)
	{
		ftowns_command("PAUSE");
	}
	else if(FTOWNS_VM_POWER_OFF==mode)
	{
		ftowns_command("POFF");
	}
}

extern "C" void ftowns_pump(void)
{
	if(nullptr==gSession)
	{
		return;
	}
	gSession->window->Interval();
	gSession->window->Render(false);
}

extern "C" int ftowns_poll_frame(unsigned int *width,unsigned int *height,const uint8_t **rgba)
{
	if(nullptr==gSession || nullptr==width || nullptr==height || nullptr==rgba)
	{
		return 0;
	}
	auto *window=gSession->bridge_window();
	std::lock_guard <std::mutex> lock(window->frameLock);
	if(window->frameSeq==window->readSeq)
	{
		return 0;
	}
	window->readSeq=window->frameSeq;
	*width=window->frameWid;
	*height=window->frameHei;
	*rgba=window->frameRGBA.data();
	return 1;
}

extern "C" void ftowns_screen_size(unsigned int *width,unsigned int *height)
{
	if(nullptr!=width)
	{
		*width=0;
	}
	if(nullptr!=height)
	{
		*height=0;
	}
	if(nullptr==gSession)
	{
		return;
	}
	auto *window=gSession->bridge_window();
	std::lock_guard <std::mutex> lock(window->frameLock);
	if(nullptr!=width)
	{
		*width=window->frameWid;
	}
	if(nullptr!=height)
	{
		*height=window->frameHei;
	}
}

extern "C" int ftowns_read_audio(int16_t *out,int frames)
{
	if(nullptr==out || 0>=frames)
	{
		return 0;
	}
	std::memset(out,0,static_cast <size_t> (frames)*2*sizeof(int16_t));

	std::lock_guard <std::mutex> lock(gMixer.lock);
	gMixer.lastPullMs.store(now_ms());
	if(nullptr==gSession)
	{
		return frames;
	}

	/* Three channels into one buffer.  All of them accumulate, so the order
	 * here is not load-bearing. */
	gMixer.cdda.mix(out,static_cast <size_t> (frames),gMixer.cddaVolL,gMixer.cddaVolR);
	const size_t fm=gMixer.fmpcm.mix(out,static_cast <size_t> (frames),256);
	gMixer.beep.mix(out,static_cast <size_t> (frames),256);

	gMixer.pulled+=frames;
	gMixer.covered+=static_cast <long long> (fm);
	return frames;
}

extern "C" void ftowns_audio_stats(long long *pulled,long long *covered,long long *dropped,long long *peak_queued)
{
	std::lock_guard <std::mutex> lock(gMixer.lock);
	const size_t peak=gMixer.fmpcm.peak_queued>gMixer.beep.peak_queued
	                  ?gMixer.fmpcm.peak_queued:gMixer.beep.peak_queued;
	if(pulled)
	{
		*pulled=gMixer.pulled;
	}
	if(covered)
	{
		*covered=gMixer.covered;
	}
	if(dropped)
	{
		*dropped=gMixer.fmpcm.dropped+gMixer.beep.dropped;
	}
	if(peak_queued)
	{
		*peak_queued=static_cast <long long> (peak);
	}
}

extern "C" void ftowns_audio_reset(void)
{
	std::lock_guard <std::mutex> lock(gMixer.lock);
	gMixer.fmpcm.clear();
	gMixer.beep.clear();
	gMixer.cdda.clear();
}

extern "C" void ftowns_set_input(const ftowns_input *state)
{
	if(nullptr==gSession || nullptr==state)
	{
		return;
	}
	std::lock_guard <std::mutex> lock(gSession->world.inputLock);
	gSession->world.input=*state;
}

extern "C" int ftowns_key(int jisKey,int down,int shift)
{
	if(nullptr==gSession)
	{
		return FTOWNS_ERR_NOT_RUNNING;
	}
	std::lock_guard <std::mutex> lock(gSession->world.inputLock);
	if(4096<gSession->world.keyEvents.size())
	{
		return FTOWNS_ERR_BAD_ARG;
	}
	gSession->world.keyEvents.push_back({jisKey,0!=down,0!=shift});
	return FTOWNS_OK;
}

extern "C" void ftowns_set_game_port(int port,int device)
{
	if(nullptr==gSession || 0>port || 2<=port)
	{
		return;
	}
	unsigned int emu=TOWNS_GAMEPORTEMU_NONE;
	switch(device)
	{
	case FTOWNS_PORT_PAD:
		emu=(0==port)?TOWNS_GAMEPORTEMU_PHYSICAL0:TOWNS_GAMEPORTEMU_PHYSICAL1;
		break;
	case FTOWNS_PORT_ANALOG_PAD:
		emu=(0==port)?TOWNS_GAMEPORTEMU_ANALOG0:TOWNS_GAMEPORTEMU_ANALOG1;
		break;
	case FTOWNS_PORT_MOUSE:
		emu=TOWNS_GAMEPORTEMU_MOUSE;
		break;
	case FTOWNS_PORT_CYBERSTICK:
		emu=TOWNS_GAMEPORTEMU_CYBERSTICK;
		break;
	case FTOWNS_PORT_NONE:
	default:
		emu=TOWNS_GAMEPORTEMU_NONE;
		break;
	}
	gSession->world.gamePort[port]=emu;
}

extern "C" void ftowns_command(const char *cmd)
{
	if(nullptr==gSession || nullptr==cmd)
	{
		return;
	}
	std::lock_guard <std::mutex> lock(gSession->world.cmdLock);
	gSession->world.commands.push_back(cmd);
}

extern "C" void ftowns_set_log_callback(ftowns_log_fn fn,void *ctx)
{
	gLogFn=fn;
	gLogCtx=ctx;
}
