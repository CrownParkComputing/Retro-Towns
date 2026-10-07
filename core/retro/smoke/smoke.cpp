/* Retro-Towns core smoke test.
 *
 * Not a front end and not a shim for one - it is the smallest thing that can
 * prove the bridge is real: start a VM on a disc, pump it for a few seconds,
 * write the frame it produced, and show that nothing in the binary reached for
 * a window system.  If this runs, the emulation core is genuinely separable
 * from Tsugaru's GUI; if it does not, no amount of ImGui will hide that.
 *
 *   smoke <rom-dir> <disc-image> [seconds] [out.ppm]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ftowns_bridge.h"

static void log_to_stderr(const char *line,void *)
{
	std::fprintf(stderr,"[bridge] %s\n",line);
}

/* RGBA8 to PPM, because a frame nobody can look at is not evidence. */
static bool write_ppm(const char *path,unsigned int wid,unsigned int hei,const uint8_t *rgba)
{
	FILE *fp=std::fopen(path,"wb");
	if(nullptr==fp)
	{
		std::fprintf(stderr,"Cannot open %s\n",path);
		return false;
	}
	std::fprintf(fp,"P6\n%u %u\n255\n",wid,hei);
	for(size_t i=0; i<static_cast <size_t> (wid)*hei; ++i)
	{
		const unsigned char px[3]={rgba[i*4],rgba[i*4+1],rgba[i*4+2]};
		std::fwrite(px,1,3,fp);
	}
	std::fclose(fp);
	return true;
}

int main(int ac,char *av[])
{
	if(ac<3)
	{
		std::fprintf(stderr,"Usage: %s <rom-dir> <disc-image> [seconds] [out.ppm]\n",av[0]);
		return 1;
	}
	const std::string romDir=av[1];
	const std::string disc=av[2];
	const int seconds=(3<ac)?std::atoi(av[3]):10;
	const char *outPath=(4<ac)?av[4]:"smoke.ppm";

	ftowns_set_log_callback(log_to_stderr,nullptr);

	/* A synthetic argv: program name, ROM directory, then the disc.  Exactly
	 * the shape Tsugaru's own command line takes, which is the point of
	 * handing the bridge a command line. */
	std::vector <std::string> argStr={"smoke",romDir,"-CD",disc,"-VERBOSE"};
	for(int i=5; i<ac; ++i)
	{
		argStr.push_back(av[i]);
	}
	std::vector <char*> argv;
	for(auto &s : argStr)
	{
		argv.push_back(s.data());
	}

	const auto started=ftowns_start(static_cast <int> (argv.size()),const_cast <const char**> (argv.data()));
	if(FTOWNS_OK!=started)
	{
		std::fprintf(stderr,"ftowns_start failed (%d)\n",started);
		return 2;
	}
	std::printf("VM started, state=%d\n",ftowns_vm_state());

	unsigned int wid=0,hei=0;
	std::vector <uint8_t> lastFrame;
	unsigned int lastWid=0,lastHei=0;
	int frames=0,audioFrames=0;
	long peak=0;

	const int pullHz=60;
	std::vector <int16_t> audio(1024*FTOWNS_AUDIO_CHANNELS);

	for(int tick=0; tick<seconds*pullHz; ++tick)
	{
		ftowns_pump();

		const uint8_t *rgba=nullptr;
		unsigned int fw=0,fh=0;
		if(0!=ftowns_poll_frame(&fw,&fh,&rgba))
		{
			++frames;
			lastFrame.assign(rgba,rgba+static_cast <size_t> (fw)*fh*4);
			lastWid=fw;
			lastHei=fh;
		}

		const auto got=ftowns_read_audio(audio.data(),1024);
		audioFrames+=got;
		/* Is there anything in the sound the core generated?  Silence here
		 * means the mixer is wired but not fed, which the eye alone misses. */
		for(int i=0; i<got*FTOWNS_AUDIO_CHANNELS; ++i)
		{
			const long mag=audio[i]<0?-audio[i]:audio[i];
			if(peak<mag)
			{
				peak=mag;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1000/pullHz));
	}

	std::printf("frames=%d  %ux%u  audio=%d frames peak=%ld  vm_state=%d\n",
	    frames,lastWid,lastHei,audioFrames,peak,ftowns_vm_state());

	if(0==frames)
	{
		std::fprintf(stderr,"No frame ever arrived.\n");
		ftowns_stop();
		return 3;
	}
	if(true!=write_ppm(outPath,lastWid,lastHei,lastFrame.data()))
	{
		ftowns_stop();
		return 4;
	}
	std::printf("wrote %s\n",outPath);

	ftowns_stop();
	std::printf("stopped, state=%d\n",ftowns_vm_state());
	return 0;
}
