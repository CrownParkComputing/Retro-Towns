/*
 * Retro-Towns - a native front end for the Tsugaru FM TOWNS core.
 *
 * SDL3 for the window, Dear ImGui for the interface, and the emulator behind
 * the plain-C bridge in core/retro/bridge. The frontend owns the window and
 * the main thread; the bridge owns the VM and UI threads and hands back a
 * finished framebuffer. Nothing here includes a Tsugaru header.
 *
 * WHY IT DOES NOT LOOK LIKE THE SATURN ONE
 * ---------------------------------------
 * Apple rejected this estate under guideline 4.3 for being one application
 * shipped several times, so "take the Saturn front end and change the colours"
 * is the one thing that must not happen. The shape here comes from the machine
 * instead:
 *
 *   - An FM TOWNS is a HOME COMPUTER with a keyboard, and a lot of its library
 *     is typed at rather than padded. The Saturn has no equivalent, so the
 *     keyboard is a first-class input here rather than an afterthought.
 *   - It boots into an operating system and stays there. The Saturn is powered
 *     on for one disc; a TOWNS is left running with a shelf of discs and a
 *     stack of floppy images beside it, so the shelf and the machine are
 *     separate places rather than the same screen.
 *   - Its BIOS is a directory of six files, not one image, and the machine will
 *     not start without all of them - which makes the first-run conversation a
 *     checklist rather than a file picker.
 *
 * What the app is, in the order a player meets it: a SHELF of disc and floppy
 * images (towns_library), a Launch page for whatever is picked off it, a Setup
 * wizard that is handed one parent folder and finds the BIOS and the discs
 * inside it, a Machine page for which model to be, and the machine itself
 * behind a button. The shelf and the machine are separate faces; the shelf is
 * where the app lives, and the machine is what it starts on demand.
 */
#include "towns_config.h"

/* Stamped in by the build script; sensible if it was not. */
#ifndef TOWNS_CORE_VERSION
#  define TOWNS_CORE_VERSION "unknown"
#endif
#ifndef TOWNS_CORE_DESC
#  define TOWNS_CORE_DESC "unknown"
#endif
#ifndef TOWNS_CORE_DATE
#  define TOWNS_CORE_DATE "unknown"
#endif

#include <SDL3/SDL.h>
/* Renames main() to SDL_main, which is the symbol SDLActivity looks up on
 * Android. A no-op everywhere else, so there is one main() and one build. */
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "ftowns_bridge.h"
}

#include "touch_pad.h"
#include "towns_keys.h"
#include "towns_pad_map.h"
#include "towns_library.h"
#include "towns_setup.h"
#include "towns_stage.h"

namespace {

/* ---- sound ----
 *
 * SDL3 hands the callback a stream to PUSH into and asks for a byte count,
 * rather than a buffer to fill, so the pull from the bridge is chunked.
 *
 * The slice has to stay well under one wave of the core's synthesiser (40 ms),
 * because that is all the bridge can promise is queued: pull 46 ms from a queue
 * holding 40 ms and the remainder is silence, which is a click at every wave
 * boundary.  The loop below makes up the requested amount in slices instead.
 */
constexpr int AUDIO_SCRATCH_FRAMES=512;
int16_t g_audio_scratch[AUDIO_SCRATCH_FRAMES*FTOWNS_AUDIO_CHANNELS];

/* -wav FILE taps the bridge's output before SDL resamples it.  Worth having,
 * because "the sound is wrong" splits into two very different bugs - the
 * emulator generated it wrong, or the host delivered it wrong - and a file that
 * sounds fine played back elsewhere settles which without anyone guessing by
 * ear. */
SDL_IOStream *g_wav=nullptr;
long long g_wav_bytes=0;

void wav_write_header(SDL_IOStream *out,long long dataBytes)
{
	const Uint32 rate=FTOWNS_AUDIO_RATE;
	const Uint32 byteRate=rate*FTOWNS_AUDIO_CHANNELS*sizeof(int16_t);
	const Uint32 chunkSize=(Uint32)(44+dataBytes);
	const Uint16 blockAlign=(Uint16)(FTOWNS_AUDIO_CHANNELS*sizeof(int16_t));
	const Uint16 bits=(Uint16)(8*sizeof(int16_t));
	auto put32=[&](Uint32 v){ SDL_WriteIO(out,&v,4); };
	auto put16=[&](Uint16 v){ SDL_WriteIO(out,&v,2); };

	put32(0x46464952);  // "RIFF"
	put32(chunkSize-8);
	put32(0x45564157);  // "WAVE"
	put32(0x20746d66);  // "fmt "
	put32(16);
	put16(1);           // PCM
	put16((Uint16)FTOWNS_AUDIO_CHANNELS);
	put32(rate);
	put32(byteRate);
	put16(blockAlign);
	put16(bits);
	put32(0x61746164);  // "data"
	put32((Uint32)dataBytes);
}

void wav_finish(void)
{
	if(nullptr==g_wav)
	{
		return;
	}
	SDL_SeekIO(g_wav,0,SDL_IO_SEEK_SET);
	wav_write_header(g_wav,g_wav_bytes);
	SDL_CloseIO(g_wav);
	g_wav=nullptr;
	SDL_Log("Captured %lld bytes of the bridge's audio.",g_wav_bytes);
}

void SDLCALL audio_callback(void *userdata,SDL_AudioStream *stream,int additional_amount,int)
{
	const bool muted=(nullptr!=userdata && 0!=*(static_cast <int*> (userdata)));

	while(0<additional_amount)
	{
		int bytes=additional_amount;
		if(bytes>(int)sizeof(g_audio_scratch))
		{
			/* Never ask for more than the scratch holds - ftowns_read_audio
			 * writes exactly what it is told. */
			bytes=(int)sizeof(g_audio_scratch);
		}
		/* Whole frames only. */
		bytes-=bytes%(FTOWNS_AUDIO_CHANNELS*(int)sizeof(int16_t));
		if(bytes<=0)
		{
			break;
		}
		const int want=bytes/(FTOWNS_AUDIO_CHANNELS*(int)sizeof(int16_t));
		const int got=ftowns_read_audio(g_audio_scratch,want);

		/* Muting happens here rather than by stopping the pull: the core paces
		 * itself on how fast its audio is consumed, so a muted machine that
		 * stopped pulling would also stop running. */
		if(muted)
		{
			SDL_memset(g_audio_scratch,0,(size_t)got*FTOWNS_AUDIO_CHANNELS*sizeof(int16_t));
		}
		if(nullptr!=g_wav && 0<got)
		{
			SDL_WriteIO(g_wav,g_audio_scratch,(size_t)got*FTOWNS_AUDIO_CHANNELS*sizeof(int16_t));
			g_wav_bytes+=(long long)got*FTOWNS_AUDIO_CHANNELS*sizeof(int16_t);
		}
		SDL_PutAudioStreamData(stream,g_audio_scratch,got*FTOWNS_AUDIO_CHANNELS*(int)sizeof(int16_t));

		if(got<want)
		{
			break;
		}
		additional_amount-=bytes;
	}
}
} /* namespace */

/* ---- controllers ----
 *
 * What the player's pad means to the machine is not a table compiled in here
 * any more - see towns_pad_map.h, which keeps it as nine named signals the
 * settings page can edit and the config file can carry.  The FM TOWNS game
 * port has two action buttons and four directions, plus RUN, PAUSE and ZOOM,
 * and that is the whole vocabulary: a control on the player's pad with nothing
 * to become is left unbound rather than mapped onto something it will not do.
 */

/* ---- dialogs and the pad ----
 *
 * The drawn pad takes its presses before ImGui does - a control on top of a
 * window must not also click that window - and while it is being arranged it
 * takes every press in the window, which is the only way to drag a control to
 * empty black.  That leaves the arrange dialog itself unable to be used: its
 * radios and sliders are presses too.  So each dialog notes the rectangle it was
 * drawn in, and a press that starts inside one of those belongs to ImGui.
 *
 * The rectangles are from the last frame, because events are read before
 * anything is drawn.  A dialog that slid under the pointer for one frame is a
 * dialog nobody is mid-drag on, so the lag costs nothing. */
namespace {
std::vector <ImVec4> g_dialogs;

void note_dialog()
{
	g_dialogs.push_back(ImVec4(ImGui::GetWindowPos().x,ImGui::GetWindowPos().y,
	                           ImGui::GetWindowSize().x,ImGui::GetWindowSize().y));
}

bool in_dialog(const ImVec2 &p)
{
	for(const ImVec4 &r : g_dialogs)
	{
		if(r.x<=p.x && p.x<=r.x+r.z && r.y<=p.y && p.y<=r.y+r.w)
		{
			return true;
		}
	}
	return false;
}
} /* namespace */

/* ---- icons ----
 *
 * Everything the bar offers while a game is running has to be readable with one
 * eye on the road, which rules out words.  The default font ships no
 * pictographs either, so these are drawn from circles and lines - the same
 * reason the on-screen pad is vector art: it has to look right at any size the
 * window ends up.
 *
 * Each takes a centre and a radius and draws inside that box, so the bar can
 * scale them with the font and stay one line tall at any DPI.
 */
namespace icon
{
	enum Kind
	{
		Disc,        /* the CD in the drive                        */
		Pad,         /* a controller - external or drawn           */
		Keyboard,    /* the machine's keyboard                     */
		Zoom,        /* how big the picture is drawn               */
		Shelf,       /* back to the launcher                       */
		Run,         /* the machine is running                     */
		Paused,      /* the machine is holding                     */
		Eye,         /* the drawn pad is on screen                 */
		EyeClosed,   /* ... and is not                             */
		Close,       /* an X - put this thing away                 */
		Towns,       /* FM Towns console                           */
		Translate    /* translation overlay                        */
	};
} /* namespace icon */

namespace {

void draw_icon(ImDrawList *dl,const ImVec2 &c,float r,int kind,ImU32 col)
{
	const float lw=SDL_max(1.5f,r*0.13f);
	switch(kind)
	{
	case icon::Disc: /* a CD: the disc, the hub, and a glint on the data band.
	                   * The glint is concentric with the rim on purpose - an
	                   * offset arc at this size reads as the bar of a
	                   * "no entry" sign, which is not the message. */
		dl->AddCircle(c,r,col,lw);
		dl->AddCircle(c,r*0.28f,col,16,lw*0.75f);
		dl->PathArcTo(c,r*0.65f,-2.45f,-1.55f);
		dl->PathStroke(col,false,lw*0.9f);
		break;
	case icon::Pad: /* the Marty's pad: a slab with a cross at one end and two
	                 * buttons at the other. */
		{
			const ImVec2 a(c.x-r,c.y-r*0.60f),b(c.x+r,c.y+r*0.60f);
			dl->AddRect(a,b,col,r*0.45f,0,lw);
			dl->AddLine(ImVec2(a.x+r*0.36f,a.y+r*0.16f),ImVec2(a.x+r*0.36f,b.y-r*0.16f),col,lw);
			dl->AddLine(ImVec2(a.x+r*0.20f,c.y),ImVec2(a.x+r*0.52f,c.y),col,lw);
			dl->AddCircleFilled(ImVec2(b.x-r*0.42f,c.y+r*0.14f),lw*1.1f,col);
			dl->AddCircleFilled(ImVec2(b.x-r*0.20f,c.y-r*0.14f),lw*1.1f,col);
		}
		break;
	case icon::Keyboard: /* a keyboard: a slab with a key grid and a long space
	                      * bar under it. */
		{
			const ImVec2 a(c.x-r,c.y-r*0.66f),b(c.x+r,c.y+r*0.66f);
			dl->AddRect(a,b,col,2.0f,0,lw);
			for(int i=0; i<4; ++i)
			{
				const float y=a.y+(b.y-a.y)*(0.22f+0.20f*(float)i);
				for(int j=0; j<5; ++j)
				{
					const float x=a.x+(b.x-a.x)*(0.14f+0.18f*(float)j);
					dl->AddCircleFilled(ImVec2(x,y),lw*0.62f,col);
				}
			}
		}
		break;
	case icon::Zoom: /* four corner brackets with a plus between them: a picture
	                  * being drawn bigger inside a window. */
		{
			const float k=r*0.45f;
			const float sx[4]={-1.0f,1.0f,1.0f,-1.0f},sy[4]={-1.0f,-1.0f,1.0f,1.0f};
			for(int i=0; i<4; ++i)
			{
				const ImVec2 p(c.x+sx[i]*r,c.y+sy[i]*r);
				dl->AddLine(p,ImVec2(p.x-sx[i]*k,p.y),col,lw);
				dl->AddLine(p,ImVec2(p.x,p.y-sy[i]*k),col,lw);
			}
			dl->AddLine(ImVec2(c.x-r*0.34f,c.y),ImVec2(c.x+r*0.34f,c.y),col,lw);
			dl->AddLine(ImVec2(c.x,c.y-r*0.34f),ImVec2(c.x,c.y+r*0.34f),col,lw);
		}
		break;
	case icon::Shelf: /* a door with an arrow pointing out of it - the way back
	                   * to the launcher. */
		{
			dl->AddRect(ImVec2(c.x-r*0.14f,c.y-r),ImVec2(c.x+r,c.y+r),col,2.0f,0,lw);
			dl->AddLine(ImVec2(c.x-r,c.y),ImVec2(c.x+r*0.06f,c.y),col,lw);
			dl->AddLine(ImVec2(c.x-r,c.y),ImVec2(c.x-r*0.42f,c.y-r*0.40f),col,lw);
			dl->AddLine(ImVec2(c.x-r,c.y),ImVec2(c.x-r*0.42f,c.y+r*0.40f),col,lw);
		}
		break;
	case icon::Run:
		dl->PathLineTo(ImVec2(c.x-r*0.55f,c.y-r*0.85f));
		dl->PathLineTo(ImVec2(c.x+r*0.85f,c.y));
		dl->PathLineTo(ImVec2(c.x-r*0.55f,c.y+r*0.85f));
		dl->PathFillConvex(col);
		break;
	case icon::Paused:
		dl->AddRectFilled(ImVec2(c.x-r*0.75f,c.y-r*0.85f),ImVec2(c.x-r*0.20f,c.y+r*0.85f),col);
		dl->AddRectFilled(ImVec2(c.x+r*0.20f,c.y-r*0.85f),ImVec2(c.x+r*0.75f,c.y+r*0.85f),col);
		break;
	case icon::Close: /* two strokes, cut square rather than rotated, so it stays
	                    * an X at a small radius. */
		dl->AddLine(ImVec2(c.x-r*0.8f,c.y-r*0.8f),ImVec2(c.x+r*0.8f,c.y+r*0.8f),col,lw);
		dl->AddLine(ImVec2(c.x+r*0.8f,c.y-r*0.8f),ImVec2(c.x-r*0.8f,c.y+r*0.8f),col,lw);
		break;
	case icon::Towns: /* FM Towns console: chassis with floppy slot and CD tray. */
		{
			const ImVec2 a(c.x-r,c.y-r*0.55f),b(c.x+r,c.y+r*0.65f);
			dl->AddRect(a,b,col,3.0f,0,lw);
			dl->AddCircle(ImVec2(c.x+r*0.4f,c.y),r*0.25f,col,16,lw*0.75f);
			dl->AddLine(ImVec2(a.x+r*0.2f,c.y),ImVec2(a.x+r*0.6f,c.y),col,lw);
		}
		break;
	case icon::Translate: /* translation speech bubble. */
		{
			const ImVec2 a(c.x-r*0.8f,c.y-r*0.6f),b(c.x+r*0.8f,c.y+r*0.2f);
			dl->AddRect(a,b,col,2.0f,0,lw);
			dl->AddLine(ImVec2(a.x+r*0.3f,b.y),ImVec2(a.x+r*0.5f,b.y+r*0.4f),col,lw);
			dl->AddLine(ImVec2(a.x+r*0.5f,b.y+r*0.4f),ImVec2(a.x+r*0.7f,b.y),col,lw);
		}
		break;
	case icon::Eye: /* open: two lids and a pupil. */
		{
			const ImVec2 l(c.x-r,c.y),rt(c.x+r,c.y);
			dl->AddBezierCubic(l,ImVec2(c.x-r*0.4f,c.y-r*0.95f),
			                   ImVec2(c.x+r*0.4f,c.y-r*0.95f),rt,col,lw);
			dl->AddBezierCubic(l,ImVec2(c.x-r*0.4f,c.y+r*0.95f),
			                   ImVec2(c.x+r*0.4f,c.y+r*0.95f),rt,col,lw);
			dl->AddCircleFilled(c,r*0.33f,col);
		}
		break;
	case icon::EyeClosed: /* shut: the upper lid dropped to meet the lower, with
	                       * two lashes saying it is a lid and not a scratch. */
		{
			const ImVec2 l(c.x-r,c.y),rt(c.x+r,c.y);
			dl->AddBezierCubic(l,ImVec2(c.x-r*0.4f,c.y+r*0.45f),
			                   ImVec2(c.x+r*0.4f,c.y+r*0.45f),rt,col,lw);
			dl->AddLine(ImVec2(c.x-r*0.55f,c.y+r*0.62f),ImVec2(c.x-r*0.30f,c.y+r*1.02f),col,lw);
			dl->AddLine(ImVec2(c.x+r*0.55f,c.y+r*0.62f),ImVec2(c.x+r*0.30f,c.y+r*1.02f),col,lw);
		}
		break;
	}
}

/* The FM Towns tower, drawn as it stood on the desk: a full-height floppy bay
 * over a CD bay, a power button and LED, and vent lines down the front.  Line
 * art like the rest of the icons, so it scales cleanly on a handheld. */
void draw_towns_computer(ImDrawList *dl,const ImVec2 &pos,const ImVec2 &size,ImU32 col)
{
	const float x0=pos.x,y0=pos.y,x1=pos.x+size.x,y1=pos.y+size.y;
	const float lw=SDL_max(1.5f,size.x*0.05f);

	/* Case. */
	dl->AddRect(ImVec2(x0,y0),ImVec2(x1,y1),col,size.x*0.08f,0,lw);

	/* Floppy bay. */
	const float fy=y0+size.y*0.16f;
	const float slot_h=size.y*0.035f;
	dl->AddRect(ImVec2(x0+size.x*0.16f,fy-slot_h),ImVec2(x1-size.x*0.16f,fy+slot_h),
	            col,lw*0.5f,0,lw*0.8f);
	dl->AddLine(ImVec2(x0+size.x*0.22f,fy),ImVec2(x1-size.x*0.22f,fy),col,lw*0.8f);
	dl->AddRectFilled(ImVec2(x1-size.x*0.27f,fy-slot_h*0.5f),
	                  ImVec2(x1-size.x*0.19f,fy+slot_h*0.5f),col);

	/* CD bay, with its tray out. */
	const float cy=y0+size.y*0.36f;
	const float dh=size.y*0.085f;
	dl->AddRect(ImVec2(x0+size.x*0.16f,cy-dh),ImVec2(x1-size.x*0.16f,cy+dh),
	            col,lw*0.5f,0,lw*0.8f);
	const float ty=cy+dh;
	dl->AddLine(ImVec2(x0+size.x*0.20f,ty),ImVec2(x1-size.x*0.20f,ty),col,lw*0.8f);
	dl->AddLine(ImVec2(x0+size.x*0.20f,ty),ImVec2(x0+size.x*0.20f,ty+size.y*0.05f),col,lw*0.8f);
	dl->AddLine(ImVec2(x1-size.x*0.20f,ty),ImVec2(x1-size.x*0.20f,ty+size.y*0.05f),col,lw*0.8f);
	dl->AddLine(ImVec2(x0+size.x*0.20f,ty+size.y*0.05f),ImVec2(x1-size.x*0.20f,ty+size.y*0.05f),col,lw*0.8f);

	/* Power button and activity LED. */
	dl->AddCircleFilled(ImVec2(x0+size.x*0.30f,y0+size.y*0.56f),size.x*0.035f,col);
	dl->AddRectFilled(ImVec2(x0+size.x*0.55f,y0+size.y*0.545f),
	                  ImVec2(x0+size.x*0.72f,y0+size.y*0.575f),col);

	/* Vents. */
	for(int i=0; i<4; ++i)
	{
		const float vy=y0+size.y*(0.68f+0.065f*(float)i);
		dl->AddLine(ImVec2(x0+size.x*0.16f,vy),ImVec2(x1-size.x*0.16f,vy),col,lw*0.7f);
	}
}

/* A button that is only a mark.  Returns true on the press, and says what it
 * was in the tooltip - the words are still there for whoever wants them, they
 * just stopped taking up room. */
bool icon_button(const char *id,int kind,float sz,ImU32 col,const char *tip)
{
	ImGui::PushID(id);
	const bool hit=ImGui::InvisibleButton("##ib",ImVec2(sz,sz));
	draw_icon(ImGui::GetWindowDrawList(),
	          ImVec2(ImGui::GetItemRectMin().x+sz*0.5f,ImGui::GetItemRectMin().y+sz*0.5f),
	          sz*0.42f,kind,col);
	if(hit)
	{
		ImGui::PopID();
		return true;
	}
	if(ImGui::IsItemHovered() && nullptr!=tip)
	{
		ImGui::SetTooltip("%s",tip);
	}
	ImGui::PopID();
	return false;
}

/* The disc in the drive, drawn rather than described: a CD with a spectral
 * sheen on its data area and the mounted title on its label.  Empty when
 * nothing is in there, because a shelf that always shows a disc is a shelf that
 * says nothing. */
void draw_disc(ImDrawList *dl,const ImVec2 &c,float r,const char *title)
{
	const bool loaded=nullptr!=title&&'\0'!=title[0];
	const ImU32 edge=IM_COL32(150,155,170,190);

	dl->AddCircleFilled(c,r,IM_COL32(30,32,40,255));
	if(loaded)
	{
		/* The data area only - a real CD has a clear centre with the label
		 * printed on it, and painting the rainbow over the middle is what makes
		 * a drawn disc look like a pie chart. */
		const ImU32 spec[6]={IM_COL32(90,200,255,70),IM_COL32(140,235,180,70),
		                     IM_COL32(235,225,120,70),IM_COL32(255,160,120,70),
		                     IM_COL32(225,120,190,70),IM_COL32(150,130,240,70)};
		const float r0=r*0.52f,r1=r*0.94f;
		for(int i=0; i<6; ++i)
		{
			const float a0=(float)i*(6.283185f/6.0f)+0.2f;
			const float a1=a0+6.283185f/6.0f*0.82f;
			for(int k=0; k<10; ++k)
			{
				const float ra=r0+(r1-r0)*(float)k/10.0f;
				dl->PathArcTo(c,ra,a0+(float)k*0.012f,a1+(float)k*0.012f,20);
				dl->PathStroke(spec[i],false,(r1-r0)*0.11f);
			}
		}
	}
	/* The label ring, the clamping ring and the hub: without them it is a
	 * coloured circle. */
	dl->AddCircleFilled(c,r*0.50f,IM_COL32(214,212,206,255));
	dl->AddCircleFilled(c,r*0.47f,IM_COL32(228,226,220,255));
	dl->AddCircle(c,r*0.20f,edge,28,1.2f);
	dl->AddCircleFilled(c,r*0.16f,IM_COL32(18,18,22,255));
	dl->AddCircle(c,r*0.16f,edge,24,1.2f);
	dl->AddCircle(c,r,edge,48,1.5f);

	if(loaded)
	{
		/* The title on the label, shortened until it fits inside the clamping
		 * ring - a disc with a sentence running off its edge is worse than a
		 * disc with a truncated one. */
		char fit[128];
		SDL_strlcpy(fit,title,sizeof(fit));
		const float wide=r*0.86f;
		while(*fit && wide<ImGui::CalcTextSize(fit).x)
		{
			char *dot=SDL_strrchr(fit,' ');
			size_t n=SDL_strlen(fit);
			if(dot && 8u<(size_t)(dot-fit))
			{
				*dot='\0';
			}
			else if(4u<n)
			{
				fit[n-4]='\0';
			}
			else
			{
				break;
			}
		}
		const ImVec2 ts=ImGui::CalcTextSize(fit);
		dl->AddText(ImVec2(c.x-ts.x*0.5f,c.y+ts.y*0.5f-r*0.26f),
		            IM_COL32(30,30,36,255),fit);
	}
	else
	{
		dl->AddText(ImVec2(c.x-ImGui::CalcTextSize("empty").x*0.5f,c.y-6.0f),
		            IM_COL32(90,92,100,255),"empty");
	}
}

/* The nine signals the machine's game port carries, as text: what each one
 * means, what the player's control is bound to now, and a Bind that listens for
 * the next press - a pad control or a keyboard key, whichever they are holding.
 * No drawing of a gamepad, because what the player owns is not the shape on the
 * machine's port and the list is the whole of the decision. */
bool padmap_page(towns::Settings &set,int &armed_sig)
{
	bool dirty=padmap::editor(set.pad,armed_sig);

	ImGui::Separator();
	if(ImGui::Button("Restore the default mapping"))
	{
		set.pad=padmap::Map::defaults();
		dirty=true;
	}
	return dirty;
}

/* The on-screen pad's settings, minus the drag-to-arrange mode (that lives in
 * its own window over the machine, where the picture is). */
bool pad_settings_section(towns::Settings &set,const std::string &cfg_dir,
                          touchpad::Overlay &pad)
{
	bool dirty=false;

	const touchpad::Profile *cur=pad.profile();
	const std::string cur_name=nullptr!=cur ? cur->name : "None";
	const touchpad::Profile *pick=cur;
	if(ImGui::BeginCombo("Pad layout",cur_name.c_str()))
	{
		const touchpad::Profile *choices[]={
			&touchpad::profile_ftowns(),
			&touchpad::profile_xbox360(),
			&touchpad::profile_generic() };
		for(const touchpad::Profile *c : choices)
		{
			if(ImGui::Selectable(c->name.c_str(),c==pick))
			{
				pick=c;
			}
		}
		ImGui::EndCombo();
	}
	if(nullptr!=pick && pick!=cur)
	{
		pad.set(pick,touchpad::Layout::load(cfg_dir,*pick));
		set.touch_pad=pick->id;
		dirty=true;
	}

	ImGui::TextUnformatted("Show it");
	ImGui::SameLine();
	dirty|=ImGui::RadioButton("On a touchscreen",&set.touch_pad_show,0);
	ImGui::SameLine();
	dirty|=ImGui::RadioButton("Always",&set.touch_pad_show,1);
	ImGui::SameLine();
	dirty|=ImGui::RadioButton("Never",&set.touch_pad_show,2);
	ImGui::SameLine();
	ImGui::TextDisabled("- a pad in the hands hides this one");
	float op=pad.layout().opacity;
	if(ImGui::SliderFloat("Opacity",&op,0.15f,1.0f))
	{
		pad.layout().opacity=op;
		pad.layout().save(cfg_dir);
	}
	return dirty;
}

/* A size authored against ImGui's 13-pixel font, in the units the interface is
 * actually being drawn at.  Almost everything here is measured off the font
 * already; these are the handful of places that named a pixel count instead,
 * and on a handheld they would otherwise stay half the size of their
 * neighbours. */
static float ui_px(float authored)
{
	return authored*(ImGui::GetFontSize()/13.0f);
}

/* Shorten a long path to fit one line: keep the head and the tail, with an
 * ellipsis between, because the tail is where the folder name lives. */
static std::string ellipsize(const std::string &s,size_t max)
{
	if(s.size()<=max)
	{
		return s;
	}
	if(max<=3)
	{
		return "...";
	}
	const size_t tail=(max-3)/2;
	const size_t head=max-3-tail;
	return s.substr(0,head)+"..."+s.substr(s.size()-tail);
}

/* The drag-to-arrange mode for the on-screen pad.  A window over the machine,
 * because controls have to be placed against the live picture. */
bool pad_arrange_window(touchpad::Overlay &pad,const std::string &cfg_dir,bool &open)
{
	bool dirty=false;

	/* Auto-sized: the panel grows and shrinks with the arrange tick, and a
	 * dialog that keeps a big empty square of its own over the game is a dialog
	 * in the way. */
	if(ImGui::Begin("Arrange the on-screen pad  -  Retro-Towns",&open,
	                ImGuiWindowFlags_AlwaysAutoResize))
	{
		note_dialog();
		ImGui::TextUnformatted("Drag the controls into place.  Tap one to size it.");
		if(pad.designer_controls(nullptr))
		{
			pad.layout().save(cfg_dir);
			dirty=true;
		}
		if(pad.has_selection())
		{
			ImGui::Separator();
			if(pad.cluster_panel())
			{
				pad.layout().save(cfg_dir);
				dirty=true;
			}
		}
		ImGui::End();
	}
	return dirty;
}

/* Screen tab: how big the picture is drawn. */
bool screen_section(towns::Settings &set)
{
	bool dirty=false;
	static const char *const kZoom[]={"Fit the window","1:1","2x","3x","4x"};
	ImGui::TextUnformatted("Picture size");
	ImGui::SameLine(ui_px(150.0f));
	if(ImGui::BeginCombo("##zoom",kZoom[set.scaling]))
	{
		for(int i=0; i<5; ++i)
		{
			if(ImGui::Selectable(kZoom[i],i==set.scaling))
			{
				set.scaling=i;
				dirty=true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled("A whole multiple keeps 8-pixel text crisp.");
	return dirty;
}

/* Ports tab: what is plugged into each game port. */
bool ports_section(towns::Settings &set)
{
	bool dirty=false;
	static const towns::PortDevice kDevices[]={
		towns::PortDevice::None,towns::PortDevice::Pad,towns::PortDevice::Mouse,
		towns::PortDevice::CyberStick,towns::PortDevice::AnalogPad };
	auto port_row=[&](const char *label,towns::PortDevice &dev,int slot)
	{
		ImGui::TextUnformatted(label);
		ImGui::SameLine(ui_px(90.0f));
		ImGui::PushID(slot);
		if(ImGui::BeginCombo("##device",towns::port_device_name(dev)))
		{
			for(const towns::PortDevice d : kDevices)
			{
				if(ImGui::Selectable(towns::port_device_name(d),d==dev))
				{
					dev=d;
					ftowns_set_game_port(slot,(int)d);
					dirty=true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
	};
	port_row("Port 1",set.port1,0);
	port_row("Port 2",set.port2,1);
	if(ImGui::SliderFloat("Stick deadzone",&set.stick_deadzone,0.0f,0.9f,"%.2f"))
	{
		dirty=true;
	}
	ImGui::TextDisabled("How far the stick travels before it counts as a direction.");
	return dirty;
}

/* The machine's keyboard, drawn.
 *
 * An FM TOWNS is a home computer and a good part of its library is typed at,
 * while the keys it has that a laptop does not - COPY and BREAK, the ten PF
 * keys, the kana row, X-WRD and ADD-W - are exactly the reason to draw one.  So
 * this is the TOWNS layout rather than a QWERTY grid, and the rows come from
 * towns::kb_layout() so that the picture and the codes cannot drift apart.
 *
 * A key is held, not tapped: the make goes out with the press and the break
 * with the release wherever the pointer has wandered by then, because a driving
 * game wants the line to stay made while the finger stays on the glass.
 *
 * SHIFT is a latch.  While it is on, the key is sent the way the core's own
 * character translator sends a shifted character - the shift bit on the event,
 * with a SHIFT make and break wrapped around it - which is what a TOWNS
 * keyboard puts in the FIFO.  The host's physical shift key needs none of this:
 * it reports itself as JIS_SHIFT in its own right, and synthesising a second
 * pair around each key would break the shift the machine still thinks is down.
 */
void keyboard_window(bool &open,bool &shift_latch,int &held)
{
	if(!open)
	{
		return;
	}

	const std::vector <std::vector <towns::KbKey>> &rows=towns::kb_layout();

	/* One unit per key width, so the rows line up the way a keyboard's do.  The
	 * main block is sixteen units across; the cursor-and-numpad row is longer
	 * than that and is allowed its own, denser unit rather than hanging off the
	 * end of the window. */
	const ImVec2 disp=ImGui::GetIO().DisplaySize;
	const float kw=SDL_min(disp.x*0.98f,1180.0f);
	const float pad=ImGui::GetStyle().WindowPadding.x;
	const float unit_main=(kw-pad*2.0f)/16.0f;
	const float kh=SDL_max(22.0f,SDL_min(40.0f,unit_main*0.82f));
	const float row_gap=SDL_max(2.0f,unit_main*0.08f);
	/* The strip along the top that says what SHIFT is doing.  The rows start
	 * under it, so the window is measured as one line plus the rows. */
	const float strip=ImGui::GetFontSize()+pad*1.2f;
	const float kth=strip+(kh+row_gap)*(float)rows.size()+pad;

	ImGui::SetNextWindowPos(ImVec2((disp.x-kw)*0.5f,disp.y-kth),ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(kw,kth),ImGuiCond_Always);
	/* ImGui draws in focus order, and the click that opened this was inside the
	 * machine window - which is therefore in front of it.  Raised every frame it
	 * is up, because a keyboard you cannot see is worse than one that cannot be
	 * covered. */
	ImGui::SetNextWindowFocus();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,8.0f);
	ImGui::SetNextWindowBgAlpha(0.96f);
	if(!ImGui::Begin("FM TOWNS keyboard",nullptr,
	                 ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
	                 ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse))
	{
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}
	note_dialog();

	ImDrawList *dl=ImGui::GetWindowDrawList();

	/* The strip along the top: what the latch is doing, and the way out. */
	ImGui::SetCursorPosX(pad);
	ImGui::SetCursorPosY(pad*0.4f);
	ImGui::TextDisabled("SHIFT %s   -   hold a key for as long as the pointer is on it",
	                    shift_latch?"latched ON":"off");
	ImGui::SameLine(kw-pad*2.0f-ImGui::GetFrameHeight());
	ImGui::SetCursorPosY(pad*0.4f);
	if(icon_button("kbclose",icon::Close,ImGui::GetFrameHeight(),
	               ImGui::GetColorU32(ImGuiCol_Text),"Put the keyboard away"))
	{
		if(towns::JIS_NULL!=held)
		{
			ftowns_key(held,0,shift_latch?1:0);
			held=towns::JIS_NULL;
		}
		open=false;
		ImGui::PopStyleVar();
		ImGui::End();
		return;
	}

	auto press=[&](int jis)
	{
		const bool sh=shift_latch && towns::JIS_SHIFT!=jis;
		if(sh)
		{
			ftowns_key(towns::JIS_SHIFT,1,0);
		}
		ftowns_key(jis,1,sh?1:0);
	};
	auto release=[&](int jis)
	{
		const bool sh=shift_latch && towns::JIS_SHIFT!=jis;
		ftowns_key(jis,0,sh?1:0);
		if(sh)
		{
			ftowns_key(towns::JIS_SHIFT,0,0);
		}
	};

	/* The pointer went up somewhere - possibly off the keyboard altogether,
	 * possibly off the window - and the line it made is still made. */
	if(towns::JIS_NULL!=held && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		release(held);
		held=towns::JIS_NULL;
	}

	for(size_t r=0; r<rows.size(); ++r)
	{
		float units=0.0f;
		for(const towns::KbKey &k : rows[r])
		{
			units+=k.wide;
		}
		const float u=SDL_min(unit_main,(kw-pad*2.0f)/units);
		float x=pad;
		const float y=strip+(kh+row_gap)*(float)r;

		ImGui::PushID((int)r);
		for(size_t i=0; i<rows[r].size(); ++i)
		{
			const towns::KbKey &k=rows[r][i];
			const float w=u*k.wide;
			ImGui::SetCursorPosX(x);
			ImGui::SetCursorPosY(y);
			ImGui::PushID((int)i);
			const bool hit=ImGui::InvisibleButton("##kb",ImVec2(SDL_max(1.0f,w-row_gap),kh));
			ImGui::PopID();

			const bool down=(k.jis==held);
			ImU32 face_col=ImGui::GetColorU32(ImGuiCol_Button);
			if(down || (shift_latch && towns::JIS_SHIFT==k.jis))
			{
				face_col=ImGui::GetColorU32(ImGuiCol_ButtonActive);
			}
			else if(ImGui::IsItemHovered())
			{
				face_col=ImGui::GetColorU32(ImGuiCol_ButtonHovered);
			}
			/* The button's own rect, rather than arithmetic on the window's - the
			 * content region starts inside the padding, and getting that wrong by
			 * a padding-width is what put the legends off their keys. */
			const ImVec2 a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
			dl->AddRectFilled(a,b,face_col,3.0f);
			if(down)
			{
				dl->AddRect(a,b,ImGui::GetColorU32(ImGuiCol_Text),1.0f);
			}

			/* The label, shrunk until it fits rather than clipped, because a key
			 * whose legend is cut off is a key nobody presses.  ImGui measures a
			 * string in whatever font is currently pushed, which is why the
			 * measuring and the drawing are inside the same PushFont. */
			const char *lab=k.label;
			float fs=ImGui::GetFontSize();
			ImGui::PushFont(nullptr,fs);
			float tw=ImGui::CalcTextSize(lab).x;
			while(tw>(b.x-a.x-4.0f) && fs>7.0f)
			{
				ImGui::PopFont();
				fs*=0.86f;
				ImGui::PushFont(nullptr,fs);
				tw=ImGui::CalcTextSize(lab).x;
			}
			int lines=1;
			for(const char *p=lab; '\0'!=*p; ++p)
			{
				if('\n'==*p)
				{
					++lines;
				}
			}
			const float lh=fs*1.1f;
			const float top=(b.y-a.y-lh*(float)lines)*0.5f;
			for(int ln=0; ln<lines; ++ln)
			{
				const char *st=lab;
				for(int q=0; q<ln; ++q)
				{
					st=SDL_strchr(st,'\n')+1;
				}
				const char *en=SDL_strchr(st,'\n');
				if(nullptr==en)
				{
					en=st+SDL_strlen(st);
				}
				const float lw2=ImGui::CalcTextSize(st,en).x;
				dl->AddText(ImVec2(a.x+((b.x-a.x)-lw2)*0.5f,a.y+top+lh*(float)ln),
				            ImGui::GetColorU32(ImGuiCol_Text),st,en);
			}
			ImGui::PopFont();
			if('\0'!=k.alt[0])
			{
				const float as_fs=SDL_max(7.0f,fs*0.72f);
				ImGui::PushFont(nullptr,as_fs);
				const ImVec2 as=ImGui::CalcTextSize(k.alt);
				dl->AddText(ImVec2(b.x-as.x-2.0f,a.y+1.0f),
				            ImGui::GetColorU32(ImGuiCol_TextDisabled),k.alt);
				ImGui::PopFont();
			}

			if(hit)
			{
				if(towns::JIS_SHIFT==k.jis)
				{
					shift_latch=!shift_latch;
				}
				else
				{
					press(k.jis);
					held=k.jis;
				}
			}
			x+=w;
		}
		ImGui::PopID();
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

} /* namespace */

int main(int argc,char *argv[])
{
	if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMEPAD))
	{
		SDL_Log("SDL_Init: %s",SDL_GetError());
		return 1;
	}

	SDL_Window *win=nullptr;
	SDL_Renderer *ren=nullptr;
	if(!SDL_CreateWindowAndRenderer("Retro-Towns",1024,768,SDL_WINDOW_RESIZABLE,&win,&ren))
	{
		SDL_Log("window: %s",SDL_GetError());
		return 1;
	}
	SDL_SetRenderVSync(ren,1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io=ImGui::GetIO();
	io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
	/* Only the title bar drags a window.  The on-screen pad is drawn over
	 * everything, including these windows, and a page that slides off when
	 * you reach for a button cannot be arranged. */
	io.ConfigWindowsMoveFromTitleBarOnly=true;

	/* ---- how big the interface is ----
	 *
	 * ImGui's built-in font is 13 pixels, sized for a debug overlay on a monitor
	 * at arm's length.  It is unreadable on a handheld, which is where this app
	 * spends most of its life.  Everything else here - the rows, the icons, the
	 * bar, the shelf - is measured off GetFontSize() and GetFrameHeight(), so
	 * the font plus the style is the one place the whole interface's scale is
	 * decided, and there is nothing per-widget to chase.
	 *
	 * The display scale is what a HiDPI desktop needs and what Android does not
	 * give: the screen density never reaches the window there, so a phone
	 * reports 1.  A touch device is therefore the reason, not the platform -
	 * and Android is assumed to be one, because SDL does not enumerate a touch
	 * screen until a finger has actually landed on it, which is after the point
	 * where this decision has to be made. */
	float ui_scale=SDL_GetWindowDisplayScale(win);
	if(0.0f>=ui_scale)
	{
		ui_scale=1.0f;
	}
	bool touch=false;
#if defined(__ANDROID__)
	touch=true;
#else
	int num_touch=0;
	SDL_free(SDL_GetTouchDevices(&num_touch));
	touch=0<num_touch;
#endif
	if(true==touch)
	{
		ui_scale=SDL_max(ui_scale,2.0f);
	}
	ImFontConfig fontCfg;
	/* Every face uses the one larger size the setup wizard settled on: the app
	 * is read at arm's length on a handheld, not at a desk. */
	fontCfg.SizePixels=SDL_min(SDL_max(13.0f*ui_scale*1.35f,17.55f),48.0f);
	io.Fonts->AddFontDefault(&fontCfg);

	/* The style's paddings, spacings and scrollbar are all in units of the
	 * 13-pixel font, so they scale with the font, not the display. */
	ImGui::GetStyle().ScaleAllSizes(fontCfg.SizePixels/13.0f);
	if(true==touch)
	{
		/* Slack round every target, for a thumb rather than a cursor.  This is
		 * not in ScaleAllSizes' vocabulary - it is a hit area, not a drawn
		 * size - so it is the one number that has to be asked for. */
		ImGui::GetStyle().TouchExtraPadding=ImVec2(4.0f,6.0f);
	}

	ImGui_ImplSDL3_InitForSDLRenderer(win,ren);
	ImGui_ImplSDLRenderer3_Init(ren);
	/* The pad drives the machine, not the interface. Turning off
	 * NavEnableGamepad is not enough: the SDL3 backend defaults to AutoFirst,
	 * which opens the first pad itself and feeds it to ImGui - the interface
	 * then navigates on its own, and the pad is already claimed by the time the
	 * emulator goes to open it. Manual with no gamepads is the documented
	 * "none"; pads are opened below, by this app, for the TOWNS.
	 * SetGamepadMode touches backend state, so it has to run after init. */
	ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual,nullptr,0);

	/* ---- where settings live ---- */
	std::string cfg_dir;
	if(char *pref=SDL_GetPrefPath("CrownParkComputing","Retro-Towns"))
	{
		cfg_dir=pref;
		SDL_free(pref);
	}
	const std::string cfg_path=cfg_dir+"retrotowns.cfg";

	towns::AppConfig cfg;
	load_app_config(cfg_path,cfg);

	/* One folder is granted, and everything is inside it.  The BIOS directory is
	 * found from there rather than configured, because a player who has pointed
	 * the app at their collection should not have to name the same folder twice
	 * - and because on Android the app could not be given six folders anyway.
	 *
	 * Both switches below override the file for this run only, which is how a
	 * developer points the app at a second collection without editing settings. */
	std::string root=cfg.library_root;
	std::string rom_dir;
	std::string rom_override;
	/* What is in the drive when the app opens.  The remembered disc is only a
	 * shelf position - restarting into a running machine is not what "I was
	 * playing this" means on a home computer that you switch off. */
	std::string disc;
	std::string shelf_position=cfg.last_disc;
	std::string wav_path;
	for(int i=1; i<argc; ++i)
	{
		if(0==SDL_strcmp(argv[i],"-root") && i+1<argc)
		{
			root=argv[++i];
		}
		else if(0==SDL_strcmp(argv[i],"-rom") && i+1<argc)
		{
			rom_override=argv[++i];
		}
		else if(0==SDL_strcmp(argv[i],"-disc") && i+1<argc)
		{
			disc=argv[++i];
		}
		else if(0==SDL_strcmp(argv[i],"-wav") && i+1<argc)
		{
			wav_path=argv[++i];
		}
		else if(0==SDL_strcmp(argv[i],"-h")||0==SDL_strcmp(argv[i],"--help"))
		{
			SDL_Log("Usage: %s [-root DIR] [-rom DIR] [-disc IMAGE] [-wav FILE]",argv[0]);
			return 0;
		}
	}
	/* Resolved once here, then by the wizard whenever the folder changes. */
	auto find_rom=[&]
	{
		const std::string found=towns::find_bios(root);
		rom_dir=rom_override.empty() ? found : rom_override;
	};
	find_rom();
	/* No BIOS is not a reason to refuse to start.  An FM TOWNS needs Fujitsu's
	 * ROMs and only the player can supply them, so with nothing configured the
	 * app opens on the wizard that asks for them. */
	if(rom_dir.empty())
	{
		SDL_Log("No BIOS yet.  An FM TOWNS will not start without Fujitsu's "
		        "ROMs, which cannot be shipped with this app.  The wizard will "
		        "ask for the folder holding FMT_SYS.ROM and the rest.");
	}

	/* ---- the library ----
	 *
	 * The shelf is the first thing the app shows and the machine is what it
	 * starts on demand, so picking a game and putting the machine back in the
	 * box are both ordinary events rather than a restart. */
	std::vector <towns::Game> games;
	std::string message;
	int loaded_game=-1;            /* index into `games`, -1 for nothing */
	size_t loaded_disc=0;

	auto rescan=[&]
	{
		games=towns::scan_library(root);
		if(games.empty())
		{
			message=root.empty()
			    ? std::string("Nothing to play yet - the wizard asks for the folder your discs are in.")
			    : "No disc images in "+root;
		}
		else
		{
			message.clear();
		}
	};
	rescan();

	auto launch=[&](const std::string &path)->bool
	{
		if(ftowns_is_running())
		{
			ftowns_stop();
		}
		std::vector <std::string> argStr={"retrotowns",rom_dir};
		if(!path.empty())
		{
			argStr.push_back(towns::Media::Floppy==towns::media_of(path) ? "-FD0" : "-CD");
			argStr.push_back(path);
		}
		/* A zero or blank setting means "say nothing", which is what the core
		 * does with an argument it was not given - so the defaults here are the
		 * core's own defaults rather than numbers this app has to keep in step
		 * with upstream. */
		if(!cfg.machine.towns_type.empty())
		{
			argStr.push_back("-TOWNSTYPE");
			argStr.push_back(cfg.machine.towns_type);
		}
		if(0<cfg.machine.cpu_freq)
		{
			argStr.push_back("-FREQ");
			argStr.push_back(std::to_string(cfg.machine.cpu_freq));
		}
		if(0<cfg.machine.mem_size)
		{
			argStr.push_back("-MEMSIZE");
			argStr.push_back(std::to_string(cfg.machine.mem_size));
		}
		if(cfg.machine.pretend_386dx)
		{
			argStr.push_back("-PRETEND386DX");
		}
		if(cfg.machine.high_fidelity)
		{
			argStr.push_back("-HIGHFIDELITY");
		}
		std::vector <char*> bridgeArgv;
		for(auto &s : argStr)
		{
			bridgeArgv.push_back(const_cast <char*> (s.c_str()));
		}

		const int started=ftowns_start((int)bridgeArgv.size(),
		                               const_cast <const char**> (bridgeArgv.data()));
		if(FTOWNS_OK!=started)
		{
			SDL_LogCritical(SDL_LOG_CATEGORY_ERROR,
			    "The FM TOWNS core could not start (%d).\n\n"
			    "Check that %s contains FMT_SYS.ROM, FMT_DOS.ROM, FMT_FNT.ROM, "
			    "FMT_F20.ROM and FMT_DIC.ROM.",started,rom_dir.c_str());
			message="The machine would not start.  Check the ROM folder.";
			return false;
		}
		ftowns_set_game_port(0,(int)cfg.machine.port1);
		ftowns_set_game_port(1,(int)cfg.machine.port2);
		ftowns_audio_reset();
		return true;
	};

	/* The core reports what went wrong by printing to stdout, which on Android
	 * is nowhere.  Routed to the platform log, a ROM that would not load or a
	 * disc that would not mount says so from a phone. */
	ftowns_set_log_callback([](const char *line,void *)
	{
		SDL_Log("[core] %s",line);
	},nullptr);

	/* A disc named on the command line is an instruction, not a suggestion;
	 * one remembered from last time is where the shelf opens. */
	std::string early_err;
	if(!disc.empty() && towns::is_archived_image(disc))
	{
		/* Unpacking blocks here, which is the right trade on a path only a
		 * developer takes: there is no window to keep alive yet. */
		const std::string unpacked=towns::stage_image(disc,towns::folder_for(root,"cd"),early_err);
		if(unpacked.empty())
		{
			SDL_Log("%s",early_err.c_str());
		}
		else
		{
			disc=unpacked;
			rescan();
		}
	}
	bool vm_up=!disc.empty() && !rom_dir.empty() && launch(disc);
	/* With nothing in the drive the app has no reason to show a black window,
	 * and with something in it the player asked for that disc, not a shelf. */
	bool at_shelf=!vm_up;

	/* Whichever image is to be the highlighted one, find it on the shelf so the
	 * Launch page opens on the game rather than on an empty drive. */
	auto locate=[&](const std::string &path)
	{
		for(size_t i=0; i<games.size(); ++i)
		{
			for(size_t d=0; d<games[i].discs.size(); ++d)
			{
				if(games[i].discs[d].path==path)
				{
					loaded_game=(int)i;
					loaded_disc=d;
					return;
				}
			}
		}
	};
	locate(vm_up ? disc : shelf_position);
	/* An image from outside the configured folder is still in the drive, and
	 * the Launch page has nothing to say about it.  Say the plain thing. */
	if(vm_up && 0>loaded_game)
	{
		message=disc;
	}

	int muted=cfg.machine.audio_muted?1:0;
	if(!wav_path.empty())
	{
		g_wav=SDL_IOFromFile(wav_path.c_str(),"wb");
		if(nullptr==g_wav)
		{
			SDL_Log("Cannot write %s: %s",wav_path.c_str(),SDL_GetError());
		}
		else
		{
			wav_write_header(g_wav,0);  // Patched with the real length on exit.
		}
	}
	SDL_AudioSpec spec{};
	spec.format=SDL_AUDIO_S16;
	spec.channels=FTOWNS_AUDIO_CHANNELS;
	spec.freq=FTOWNS_AUDIO_RATE;
	SDL_AudioStream *astream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,audio_callback,&muted);
	if(nullptr==astream)
	{
		/* Not fatal. The machine runs, it is just silent, and a picture with
		 * no sound is worth more than a refusal to start. */
		SDL_Log("audio: %s - running silent",SDL_GetError());
	}
	else
	{
		/* SDL_OpenAudioDeviceStream hands back a paused stream. */
		SDL_ResumeAudioStreamDevice(astream);
	}

	/* ---- pads ----
	 *
	 * Enumerated up front and re-enumerated on SDL_EVENT_GAMEPAD_ADDED,
	 * because a pad plugged in after the app started is the common case on a
	 * desktop and "restart it and it will work" is not an answer. */
	std::vector <SDL_Gamepad*> pads;
	auto open_pads=[&]{
		for(SDL_Gamepad *g : pads)
		{
			if(g)
			{
				SDL_CloseGamepad(g);
			}
		}
		pads.clear();
		int count=0;
		if(SDL_JoystickID *ids=SDL_GetGamepads(&count))
		{
			for(int i=0; i<count; ++i)
			{
				if(SDL_Gamepad *g=SDL_OpenGamepad(ids[i]))
				{
					pads.push_back(g);
				}
			}
			SDL_free(ids);
		}
	};
	open_pads();

	/* ---- the on-screen pad ---- */
	touchpad::Overlay pad;
	const touchpad::Profile *prof=touchpad::profile_by_id(cfg.machine.touch_pad);
	if(nullptr==prof)
	{
		prof=&touchpad::profile_ftowns();
	}
	pad.set(prof,touchpad::Layout::load(cfg_dir,*prof));

	/* Whether this machine has a screen you can touch.  Read once: a desktop
	 * does not grow one mid-game, and the answer only decides which of the
	 * three show modes "on a touchscreen" is, which the player can override. */
	int ntouch=0;
	SDL_TouchID *touches=SDL_GetTouchDevices(&ntouch);
	const bool has_touch=(nullptr!=touches && 0<ntouch);
	SDL_free(touches);

	/* Whether the drawn pad is on screen at all.  "Never" is the player's
	 * answer to a game that wants the whole picture, and "on a touchscreen"
	 * is the reason a desktop with a pad on the desk does not get one laid
	 * over the game.  Arranging overrides both: ticking it is a request to
	 * see the controls. */
	auto pad_visible=[&]
	{
		/* The pad is a control on top of a picture.  With the machine off
		 * there is no picture, and buttons over a shelf of games would only
		 * be in the way. */
		if(!vm_up)
		{
			return false;
		}
		if(pad.editing())
		{
			return true;
		}
		/* The shelf is not a picture to put controls over.  Arranging is the
		 * exception, and it was answered two lines above. */
		if(at_shelf)
		{
			return false;
		}
		/* Either/or.  A pad in the hands beats the pad on the glass: the two
		 * drive the same nine signals, so a drawn button left under a thumb
		 * that is already holding the real one is a control nobody meant to
		 * press.  Arranging is the exception above, because ticking it is a
		 * request to see the drawn controls. */
		if(!pads.empty())
		{
			return false;
		}
		if(2==cfg.machine.touch_pad_show)
		{
			return false;
		}
		return 1==cfg.machine.touch_pad_show || has_touch;
	};

	/* What the overlay is holding.  Kept apart from the physical pad rather
	 * than written straight into `input`, because both drive the same nine
	 * signals and the last writer would otherwise win - which reads as a
	 * control that works until the other hand touches something. */
	unsigned int overlay_bits=0;
	touchpad::Sink pad_sink;
	pad_sink.directions=[&](bool up,bool down,bool left,bool right)
	{
		overlay_bits&=~(FTOWNS_PAD_UP|FTOWNS_PAD_DOWN|FTOWNS_PAD_LEFT|FTOWNS_PAD_RIGHT);
		if(up){overlay_bits|=FTOWNS_PAD_UP;}
		if(down){overlay_bits|=FTOWNS_PAD_DOWN;}
		if(left){overlay_bits|=FTOWNS_PAD_LEFT;}
		if(right){overlay_bits|=FTOWNS_PAD_RIGHT;}
	};
	pad_sink.action=[&](const std::string &id,bool down)
	{
		static const struct {const char *id;unsigned int bit;} kActions[]={
			{"a",FTOWNS_PAD_A},
			{"b",FTOWNS_PAD_B},
			{"run",FTOWNS_PAD_RUN},
			{"pause",FTOWNS_PAD_PAUSE},
			{"zoom",FTOWNS_PAD_ZOOM},
		};
		for(const auto &a : kActions)
		{
			if(a.id==id)
			{
				if(down){overlay_bits|=a.bit;}
				else{overlay_bits&=~a.bit;}
				return;
			}
		}
	};
	/* An id the overlay knows and this table does not is a control that does
	 * nothing, which is what happens when the profile and the machine disagree;
	 * saying it once at startup is more use than discovering it mid-game. */
	for(const auto &c : prof->clusters)
	{
		for(const auto &b : c.buttons)
		{
			if("a"!=b.id && "b"!=b.id && "run"!=b.id && "pause"!=b.id && "zoom"!=b.id)
			{
				SDL_Log("On-screen button \"%s\" has no TOWNS signal and will do nothing.",
				    b.label.c_str());
			}
		}
	}

	/* Keys bound to pad signals, so the machine can be played without a pad.
	 * A key that is bound this way is deliberately not also typed at the
	 * machine - the two paths are checked in that order below. */
	unsigned int key_bits=0;

	ftowns_input input{};

	Uint64 next_audio_check=SDL_GetTicks()+5000;
	long long last_pulled=0,last_covered=0;

	SDL_Texture *frame=nullptr;
	int frame_w=0,frame_h=0;
	unsigned int pic_w=0,pic_h=0;
	/* Said once, not per frame - see the upload. */
	bool upload_warned=false;
	int last_vm_state=-1;
	bool running=true;

	/* The two windows the pad row opens straight onto - the mapping of a pad in
	 * the hands, and the layout of the pad drawn on glass.  Each is one window
	 * with one job, because a player who has named a device should land on that
	 * device and nothing else. */
	bool pad_edit_open=false;
	int armed_sig=-1;
	bool controls_dirty=false;
	bool pad_editing=false;
	/* The drawn keyboard, and the two things it needs to remember between frames:
	 * whether its SHIFT key is latched, and which key is currently held down by
	 * the pointer.  Held rather than tapped, because a driving game wants the
	 * line to stay made while the finger stays on the glass. */
	bool keyboard_open=false;
	bool kb_shift=false;
	int kb_held=towns::JIS_NULL;
	/* Where the picture landed last frame, for the zoom maths and for nothing
	 * else - the pad used to be laid out over it, which put a thumb on the game
	 * the player was trying to look at. */
	ImVec2 pic_pos(0.0f,0.0f),pic_size(1.0f,1.0f);
	/* What the on-screen pad is laid out over: the client rect minus the status
	 * bar, not the picture.  The pad belongs in the black around the game, and
	 * it cannot do that if its coordinates are the game's. */
	ImVec2 pad_area(1.0f,1.0f);
	/* What "fit the window" works out to this frame, so the zoom button can tell
	 * which of its rungs the window can actually show. */
	float fit_scale=1.0f;

	/* ---- the shelf ----
	 *
	 * Two more pieces of state than the machine needed: which FACE of the
	 * shelf is showing, and whether the player is looking at the shelf or at
	 * the machine.  The second is not derived from the first - a running
	 * machine can be left behind to go and change a disc, and it waits paused
	 * while it is. */
	enum class Face { Launch, Library, Setup, Machine, Settings };

	/* The setup wizard is four steps, walked in order: explain, choose the
	 * folder, see what was made and what goes in it, then check what was found.
	 * It opens on the first step when the app has no folder yet, when an update
	 * changed the wizard's idea of the layout, or when the player asks for it
	 * from Settings. */
	enum class Wiz { Welcome, Folder, Layout, Review };
	Wiz wiz=Wiz::Welcome;

	/* First run (no folder) and "an update changed the wizard" both reopen the
	 * wizard.  A missing BIOS is NOT enough on its own: the player can browse a
	 * library of discs before finding the ROMs, and the Launch page explains
	 * that the machine will not start without them. */
	const bool need_setup=root.empty() || cfg.setup_version!=towns::kSetupVersion;
	/* Until the folder is chosen and a BIOS is found, the app stays on the
	 * setup wizard. */
	Face face=(need_setup || rom_dir.empty()) ? Face::Setup : Face::Library;
	char search[96]={};
	char letter='\0';                       /* 0 means no filter */
	char root_edit[512]={};
	SDL_strlcpy(root_edit,root.c_str(),sizeof(root_edit));
	bool boot_now=false;
	std::string boot_path;
	bool translate_to_english=false;
	auto stop_machine=[&]()
	{
		if(!boot_path.empty() && towns::is_archived_image(boot_path))
		{
			const std::string staged_dir=towns::stage_dir_for(boot_path,towns::folder_for(root,"cd"));
			SDL_RemovePath(staged_dir.c_str());
		}
		ftowns_stop();
	};
	/* Redone when the folder changes, not every frame: it is a disk walk, and
	 * nothing else on this page moves underneath it. */
	towns::BiosCheck bios;
	auto recheck=[&]
	{
		find_rom();
		bios=towns::check_bios(towns::bios_candidate(root));
	};
	recheck();
	/* Make the bios/cd/zip folders the wizard promises, when they are missing.
	 * Idempotent, and only when a folder is actually configured. */
	if(!root.empty())
	{
		towns::ensure_layout(root);
	}

	/* Moving the library is not a two-step process.  A folder chosen from the
	 * picker, or typed into the field and left with, is the folder the app
	 * uses - and it is in the settings file before the player can press
	 * anything, because "I set it and it forgot" is the worst way for a
	 * launcher to behave. */
	auto apply_root=[&](std::string chosen)
	{
		while(!chosen.empty() && ('/'==chosen.back()||'\\'==chosen.back()))
		{
			chosen.pop_back();
		}
		if(chosen.empty() || chosen==root)
		{
			return;
		}
		root=chosen;
		cfg.library_root=root;
		SDL_strlcpy(root_edit,root.c_str(),sizeof(root_edit));
		recheck();
		towns::ensure_layout(root);
		save_app_config(cfg_path,cfg);
		rescan();
		if(!rom_dir.empty())
		{
			message.clear();
		}
	};

	/* ---- unpacking, then powering up ----
	 *
	 * The core mounts a .cue and nothing else, so a zipped or CHD'd rip has to
	 * become files on disk first.  That is hundreds of megabytes, which is far
	 * too long to do inside the frame that is supposed to be drawing the
	 * progress bar, so it happens on its own thread and the shelf asks for the
	 * answer every frame. */
	std::atomic <bool> stage_busy{false};
	std::atomic <std::int64_t> stage_done{0},stage_total{0};
	std::string stage_what;                 /* the archive being unpacked      */
	std::string staged,staged_err;          /* handed over when stage_busy drops */
	std::thread stage_thread;

	auto finish_boot=[&](bool up)
	{
		vm_up=up;
		at_shelf=!up;
		if(!up)
		{
			face=Face::Launch;
			return;
		}
		/* The old frame is the previous game's title screen, and showing it
		 * while the new machine boots reads as the wrong disc. */
		if(frame)
		{
			SDL_DestroyTexture(frame);
			frame=nullptr;
			frame_w=frame_h=0;
		}
		pic_w=pic_h=0;
	};

	auto begin_boot=[&](const std::string &path)
	{
		if(!towns::is_archived_image(path))
		{
			finish_boot(launch(path));
			return;
		}
		if(stage_busy)
		{
			return;   /* one unpacking at a time */
		}
		const std::string cd_dir=towns::folder_for(root,"cd");
		stage_what=path;
		stage_done=0;
		stage_total=0;
		staged.clear();
		staged_err.clear();
		stage_busy=true;
		if(stage_thread.joinable())
		{
			stage_thread.join();
		}
		stage_thread=std::thread(
		    [path,cd_dir,&staged,&staged_err,&stage_busy,&stage_done,&stage_total]
		    {
			    staged=towns::stage_image(
			        path,cd_dir,staged_err,
			        [&stage_done,&stage_total](std::int64_t done,std::int64_t total)
			        {
				        stage_done.store(done);
				        stage_total.store(total);
			        });
			    stage_busy.store(false);
		    });
	};

	auto shelf_button=[&](const char *label,Face f)
	{
		if(ImGui::Button(label,ImVec2(-FLT_MIN,40.0f*ImGui::GetFontSize()/16.0f)))
		{
			if(face!=f)
			{
				face=f;
				/* Leaving the pad page cancels a half-made binding, so the next
				 * key the player presses is not swallowed by it. */
				armed_sig=-1;
				/* Opening Setup from the rail starts the wizard at the top. */
				if(Face::Setup==f)
				{
					wiz=Wiz::Welcome;
				}
			}
		}
		if(face==f)
		{
			/* The pressed one is the page being read, and a rail of four
			 * identical-looking buttons says nothing about which that is. */
			ImGui::GetWindowDrawList()->AddRectFilled(
			    ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),
			    IM_COL32(0x34,0xD9,0xC4,48),4.0f);
		}
	};

	auto draw_shelf=[&](int win_w,int win_h)
	{
		ImGui::SetNextWindowPos(ImVec2(0,0));
		ImGui::SetNextWindowSize(ImVec2((float)win_w,(float)win_h));
		ImGui::Begin("##shelf",nullptr,
		             ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
		             ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
		             ImGuiWindowFlags_NoBringToFrontOnFocus);

		const bool locked=root.empty() || rom_dir.empty();
		if(locked)
		{
			/* The setup wizard is the whole app until a disc has booted once. */
			face=Face::Setup;
		}

		const float rail_w=210.0f;
		ImGui::BeginChild("##rail",ImVec2(rail_w,0),ImGuiChildFlags_Borders);
		ImGui::Spacing();
		{
			const float w = ImGui::GetContentRegionAvail().x;
			const char *logo_text = "RETRO-TOWNS";
			ImGui::SetCursorPosX((w - ImGui::CalcTextSize(logo_text).x) * 0.5f + ImGui::GetCursorPosX());
			ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.77f, 1.0f), "%s", logo_text);
		}
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		if(!locked)
		{
			shelf_button("Launch",Face::Launch);
			shelf_button("Library",Face::Library);
			shelf_button("Machine",Face::Machine);
			shelf_button("Setup",Face::Setup);
			shelf_button("Settings",Face::Settings);
		}
		ImGui::Separator();
		if(!locked && vm_up)
		{
			/* A running machine is a place you can leave and come back to, not
			 * a state the app has to be in.  It waits paused while you are not
			 * looking at it, so nothing plays or records in the dark. */
			ImGui::Spacing();
			if(ImGui::Button("Back to the machine",
			                 ImVec2(-FLT_MIN,40.0f*ImGui::GetFontSize()/16.0f)))
			{
				at_shelf=false;
				armed_sig=-1;
				ftowns_set_run_mode(FTOWNS_VM_RUN);
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##face",ImVec2(0,0));

		if(Face::Setup==face)
		{
			const towns::LayoutReport layout=towns::scan_layout(root);

			/* The wizard can be taller than a handheld screen; let it scroll
			 * rather than clip the buttons at the bottom. */
			ImGui::BeginChild("##setupscroll",ImVec2(0,0));

			const char *step_name="";
			int step=0;
			switch(wiz)
			{
			case Wiz::Welcome: step_name="Welcome"; step=1; break;
			case Wiz::Folder:  step_name="Choose the folder"; step=2; break;
			case Wiz::Layout:  step_name="Your folders"; step=3; break;
			case Wiz::Review:  step_name="Check what was found"; step=4; break;
			}
			ImGui::TextUnformatted("Setup wizard");
			ImGui::TextDisabled("Step %d of 4  -  %s",step,step_name);
			ImGui::Separator();
			ImGui::Spacing();

			/* Always show the folder and what is in it, whatever step the player
			 * is on - this is the running "is it set up yet" answer. */
			if(!root.empty())
			{
				ImGui::TextDisabled("Folder: %s",ellipsize(root,48).c_str());
				if(!layout.root_exists)
				{
					ImGui::TextColored(ImVec4(0.91f,0.63f,0.36f,1.0f),
					                   "Can't read this folder.");
				}
				else
				{
					std::string summary=std::to_string(games.size())+" game"+(1==games.size()?"":"s");
					for(const auto &f : layout.folders)
					{
						summary+="   |   "+f.kind+"/ ";
						summary+=(f.exists ? std::to_string(f.files+f.dirs) : std::string("-"));
					}
					ImGui::TextDisabled("%s",summary.c_str());
				}
				ImGui::Separator();
				ImGui::Spacing();
			}

			if(Wiz::Welcome==wiz)
			{
				ImGui::TextWrapped("Choose one folder for your FM Towns files.");
				ImGui::BulletText("bios  -  Fujitsu ROMs");
				ImGui::BulletText("cd    -  disc images");
				ImGui::BulletText("zip   -  zipped rips");
				ImGui::Spacing();
				ImGui::TextWrapped("The BIOS is yours to supply.");
				ImGui::Spacing();
				if(ImGui::Button("Choose a folder"))
				{
					wiz=Wiz::Folder;
				}
				if(!root.empty())
				{
					ImGui::SameLine();
					if(ImGui::Button("Keep the current folder"))
					{
						/* Confirming the folder that is already set is finishing
						 * the reconfirm the wizard opened for. */
						cfg.setup_version=towns::kSetupVersion;
						save_app_config(cfg_path,cfg);
						face=Face::Library;
					}
				}
			}
			else if(Wiz::Folder==wiz)
			{
				ImGui::TextWrapped("Pick the folder, or make one first with your file manager.");
				ImGui::Spacing();

				if(towns::saf_available())
				{
					if(ImGui::Button("Grant folder access..."))
					{
						towns::saf_pick();
					}
					ImGui::TextWrapped("Android then makes bios/, cd/ and zip/ inside it.");

					auto trees=towns::saf_trees();
					if(!trees.empty())
					{
						ImGui::Spacing();
						ImGui::TextUnformatted("Folders already granted:");
						for(const auto &t : trees)
						{
							ImGui::BulletText("%s",t.name.c_str());
							ImGui::SameLine();
							if(!t.path.empty())
							{
								if(ImGui::Button(("Use##"+t.uri).c_str()))
								{
									apply_root(t.path);
									if(!root.empty()) wiz=Wiz::Layout;
								}
								ImGui::SameLine();
							}
							std::string fbtn="Forget##"+t.uri;
							if(ImGui::Button(fbtn.c_str()))
							{
								towns::saf_forget(t.uri);
							}
						}
					}

					ImGui::Spacing();
					ImGui::TextWrapped("Or use one of these:");
					for(const auto &c : towns::candidate_roots())
					{
						ImGui::Bullet();
						ImGui::SameLine();
						if(ImGui::Button(("Use##"+c).c_str()))
						{
							apply_root(c);
							if(!root.empty()) wiz=Wiz::Layout;
						}
						ImGui::SameLine();
						ImGui::TextDisabled("%s",ellipsize(c,52).c_str());
					}
				}
				else
				{
					ImGui::TextUnformatted("Library folder");
					ImGui::SetNextItemWidth(- ImGui::CalcTextSize("Browse").x
					                         - ImGui::GetStyle().ItemSpacing.x - 1.0f);
					if(ImGui::InputText("##root",root_edit,(size_t)sizeof(root_edit)) &&
					   ImGui::IsItemDeactivatedAfterEdit())
					{
						apply_root(root_edit);
					}
					ImGui::SameLine();
					if(ImGui::Button("Browse") && !towns::pick_in_progress())
					{
						towns::begin_pick_folder();
					}
					if(towns::pick_in_progress())
					{
						ImGui::SameLine();
						ImGui::TextDisabled("choosing...");
					}
				}

				std::string picked;
				if(towns::take_pick(picked))
				{
					const std::string before=root;
					apply_root(picked);
					if(!root.empty() && root!=before)
					{
						wiz=Wiz::Layout;
					}
				}

				ImGui::Spacing();
				if(root.empty())
				{
					ImGui::TextColored(ImVec4(0.91f,0.63f,0.36f,1.0f),
					                   "No folder chosen yet.");
				}

				ImGui::Spacing();
				if(ImGui::Button("Back"))
				{
					wiz=Wiz::Welcome;
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(root.empty());
				if(ImGui::Button("Next"))
				{
					towns::ensure_layout(root);
					wiz=Wiz::Layout;
				}
				ImGui::EndDisabled();
			}
			else if(Wiz::Layout==wiz)
			{
				ImGui::TextWrapped("Three folders. Put the right thing in each.");
				ImGui::Spacing();
				for(const auto &f : layout.folders)
				{
					const bool ok=f.exists;
					ImGui::TextColored(ok ? ImVec4(0.55f,0.85f,0.60f,1.0f)
					                      : ImVec4(0.91f,0.42f,0.34f,1.0f),
					                   "%s/  %s",f.kind.c_str(),
					                   ok ? "ready" : "could not create");
				}
				ImGui::Spacing();
				ImGui::BulletText("bios/  Fujitsu ROMs");
				ImGui::BulletText("cd/    .cue + .bin, or .iso");
				ImGui::BulletText("zip/   unpacked into cd/");
				ImGui::Spacing();
				if(ImGui::Button("Back"))
				{
					wiz=Wiz::Folder;
				}
				ImGui::SameLine();
				if(ImGui::Button("Check what was found"))
				{
					recheck();
					rescan();
					wiz=Wiz::Review;
				}
			}
			else
			{
				/* ---- the BIOS ---- */
				ImGui::TextUnformatted("BIOS");
				if(!bios.exists)
				{
					ImGui::TextColored(ImVec4(0.95f,0.45f,0.35f,1.0f),
					                   "No BIOS folder found.");
					ImGui::TextWrapped("Put the Fujitsu ROMs in bios/.");
				}
				else
				{
					if(bios.combined)
					{
						ImGui::TextDisabled("Using FMT_ALL.ROM.");
					}
					for(const auto &r : bios.rows)
					{
						if(towns::ROM_MAY==r.need.need && 2!=r.state) continue;
						ImGui::PushID(r.need.name);
						if(2==r.state)
							ImGui::TextColored(ImVec4(0.55f,0.85f,0.60f,1.0f),
							                   "  %s  OK",r.need.name);
						else if(1==r.state)
							ImGui::TextColored(ImVec4(0.91f,0.63f,0.36f,1.0f),
							                   "  %s  wrong size",r.need.name);
						else
							ImGui::TextColored(ImVec4(0.91f,0.42f,0.34f,1.0f),
							                   "  %s  missing",r.need.name);
						ImGui::PopID();
					}
					if(0<bios.missing)
						ImGui::TextColored(ImVec4(0.91f,0.42f,0.34f,1.0f),
						                   "Machine can't start yet.");
					else
						ImGui::TextColored(ImVec4(0.55f,0.85f,0.60f,1.0f),
						                   "Ready.");
				}

				ImGui::Spacing();
				if(ImGui::Button("Rescan"))
				{
					recheck();
					rescan();
				}
				ImGui::SameLine();
				if(ImGui::Button("Back"))
				{
					wiz=Wiz::Layout;
				}
				ImGui::SameLine();
				const bool can_finish=!rom_dir.empty();
				ImGui::BeginDisabled(!can_finish);
				if(ImGui::Button("Finish"))
				{
					cfg.setup_version=towns::kSetupVersion;
					save_app_config(cfg_path,cfg);
					face=Face::Library;
				}
				ImGui::EndDisabled();
				if(!can_finish)
				{
					ImGui::TextWrapped("Add the BIOS before finishing.");
				}
			}
			ImGui::EndChild();
		}
		else if(Face::Machine==face)
		{
			ImGui::TextUnformatted("Machine");
			ImGui::Spacing();
			ImGui::TextWrapped("Which FM TOWNS to be.  A title that wants a "
			                   "machine it is not running on does not explain "
			                   "itself - it shows a black screen - so these are "
			                   "here for the game that will not boot, not to be "
			                   "tuned.  Changing them restarts the machine.");
			ImGui::Spacing();

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Model");
			ImGui::SameLine(ui_px(160.0f));
			ImGui::SetNextItemWidth(-1.0f);
			int model=0;
			for(int i=1; i<towns::kTownsModelCount; ++i)
			{
				if(cfg.machine.towns_type==towns::kTownsModels[i].name)
				{
					model=i;
					break;
				}
			}
			const char *cur=cfg.machine.towns_type.empty()
			                 ? "Core default" : cfg.machine.towns_type.c_str();
			if(ImGui::BeginCombo("##model",cur))
			{
				for(int i=0; i<towns::kTownsModelCount; ++i)
				{
					const bool sel=i==model;
					if(ImGui::Selectable(towns::kTownsModels[i].name[0]
					                      ? towns::kTownsModels[i].name
					                      : "Core default",sel))
					{
						cfg.machine.towns_type=towns::kTownsModels[i].name;
					}
					if(sel)
					{
						ImGui::SetItemDefaultFocus();
					}
					ImGui::TextDisabled("%s",towns::kTownsModels[i].what);
				}
				ImGui::EndCombo();
			}
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("CPU clock");
			ImGui::SameLine(ui_px(160.0f));
			ImGui::SetNextItemWidth(ui_px(170.0f));
			{
				char fbuf[16]={};
				if(0<cfg.machine.cpu_freq)
				{
					snprintf(fbuf,sizeof(fbuf),"%d",cfg.machine.cpu_freq);
				}
				if(ImGui::InputTextWithHint("##freq","as the model ships",
				                            fbuf,(size_t)sizeof(fbuf)))
				{
					cfg.machine.cpu_freq=atoi(fbuf);
					if(cfg.machine.cpu_freq<0 || 200<cfg.machine.cpu_freq)
					{
						cfg.machine.cpu_freq=0;
					}
				}
			}
			ImGui::SameLine();
			ImGui::TextDisabled("MHz");
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Memory");
			ImGui::SameLine(ui_px(160.0f));
			ImGui::SetNextItemWidth(ui_px(170.0f));
			{
				char mbuf[16]={};
				if(0<cfg.machine.mem_size)
				{
					snprintf(mbuf,sizeof(mbuf),"%d",cfg.machine.mem_size);
				}
				if(ImGui::InputTextWithHint("##mem","as the model ships",
				                            mbuf,(size_t)sizeof(mbuf)))
				{
					cfg.machine.mem_size=atoi(mbuf);
					if(cfg.machine.mem_size<0 || 64<cfg.machine.mem_size)
					{
						cfg.machine.mem_size=0;
					}
				}
			}
			ImGui::SameLine();
			ImGui::TextDisabled("MB");
			ImGui::Spacing();
			ImGui::Checkbox("Pretend to be a 386DX",&cfg.machine.pretend_386dx);
			ImGui::TextWrapped("Some titles check the CPU and misbehave when they "
			                   "find something faster than the box they shipped "
			                   "for.");
			ImGui::Checkbox("High fidelity 80386 core",&cfg.machine.high_fidelity);
			ImGui::TextWrapped("Considerably slower, and some titles only run "
			                   "correctly on it.");
			ImGui::Spacing();
			if(ImGui::Button("Save"))
			{
				save_app_config(cfg_path,cfg);
				/* The model, the clock and the memory size are command-line
				 * arguments to the core, not things it can be told while it
				 * runs, so a change means powering it down and back up. */
				if(vm_up)
				{
					boot_path=0<=loaded_game
					          ? (games[loaded_game].disc(loaded_disc)
					             ? games[loaded_game].disc(loaded_disc)->path
					             : std::string())
					          : std::string();
					boot_now=true;
					message="Restarting the machine with the new settings.";
				}
			}
		}
		else if(Face::Library==face)
		{
			ImGui::SetNextItemWidth(ui_px(260.0f));
			ImGui::InputTextWithHint("##search","Search",search,(size_t)sizeof(search));
			ImGui::SameLine();
			if(ImGui::Button("Rescan"))
			{
				rescan();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%zu on the shelf",games.size());

			/* A-Z in one strip.  A folder of ninety images is not readable
			 * without it, and a search box nobody types into is not either. */
			/* A-Z in one strip, spread over the whole width.  At their natural
			 * button size the letters ended in a ragged edge halfway across the
			 * page and needed two rows to hold them; sized to the column they
			 * read as the index they are. */
			ImGui::Separator();
			{
				const int kSlots=27;                  /* '#' plus A-Z */
				const float gap=ImGui::GetStyle().ItemSpacing.x;
				const float avail=ImGui::GetContentRegionAvail().x;
				const float cell=(avail-gap*(kSlots-1))/(float)kSlots;
				for(int slot=0; slot<kSlots; ++slot)
				{
					const char c=(0==slot) ? '#' : (char)('A'+slot-1);
					if(0!=slot)
					{
						ImGui::SameLine();
					}
					const std::string one(1,c);
					ImGui::PushStyleColor(ImGuiCol_Text,
					    ('\0'==letter||letter==c) ? ImVec4(1,1,1,1)
					                              : ImVec4(0.42f,0.42f,0.42f,1.0f));
					if(ImGui::Button(one.c_str(),ImVec2(cell,0)))
					{
						letter=(letter==c) ? '\0' : c;
					}
					ImGui::PopStyleColor();
				}
			}
			ImGui::Separator();

			/* One game to a row.  The cards were sized for box art this app does
			 * not fetch yet, and until then a grid of empty grey plates showed a
			 * shorter title than one line does - the long ones wrapped to three
			 * lines and the shelf turned into a jigsaw. */
			const float avail=ImGui::GetContentRegionAvail().x;
			const float row_h=SDL_max(ImGui::GetFrameHeight(),
			                          ImGui::GetFontSize()*2.2f);
			int shown=0;
			for(size_t i=0; i<games.size(); ++i)
			{
				const towns::Game &g=games[i];
				if('\0'!=search[0] && nullptr==SDL_strcasestr(g.title.c_str(),search))
				{
					continue;
				}
				if('\0'!=letter && g.initial!=letter)
				{
					continue;
				}
				ImGui::PushID((int)i);
				const ImVec2 p0=ImGui::GetCursorScreenPos();
				ImDrawList *dl=ImGui::GetWindowDrawList();
				const ImVec2 p1(p0.x+avail,p0.y+row_h);
				if(loaded_game==(int)i)
				{
					dl->AddRectFilled(p0,p1,IM_COL32(0x34,0xD9,0xC4,40),4.0f);
				}
				ImGui::InvisibleButton("##pick",ImVec2(avail,row_h));
				if(ImGui::IsItemHovered())
				{
					dl->AddRect(p0,p1,IM_COL32(0x34,0xD9,0xC4,255),4.0f,0,2.0f);
				}
				if(ImGui::IsItemClicked())
				{
					loaded_game=(int)i;
					loaded_disc=0;
					face=Face::Launch;
					message.clear();
				}
				/* The media first, as a glyph: a CD and a 3.5" floppy are
				 * different problems to a player, and the word says it in less
				 * room than it takes to read. */
				draw_icon(dl,ImVec2(p0.x+row_h*0.5f,p0.y+row_h*0.5f),
				          row_h*0.28f,icon::Disc,IM_COL32(0x9a,0xa4,0xb0,255));

				std::string tail=towns::media_name(g.media);
				if(1<g.discs.size())
				{
					tail+="  "+std::to_string(g.discs.size())+" discs";
				}
				const ImVec2 ss=ImGui::CalcTextSize(tail.c_str());
				const float left=p0.x+row_h+8.0f;
				const float title_max=SDL_max(40.0f,p1.x-ss.x-18.0f-left);

				/* Long enough titles are real - the library names are catalog
				 * entries, not identifiers - so the row clips one rather than
				 * letting it run under the media tag.  Only the ones that do not
				 * fit are measured character by character. */
				std::string fit=g.title;
				const float dots=ImGui::CalcTextSize("...").x;
				if(ImGui::CalcTextSize(fit.c_str()).x>title_max)
				{
					const char *begin=g.title.c_str();
					const char *end=begin+g.title.size();
					while(end>begin &&
					      ImGui::CalcTextSize(begin,end).x>title_max-dots)
					{
						--end;
					}
					fit.assign(begin,end);
					fit+="...";
				}
				const ImVec2 ts=ImGui::CalcTextSize(fit.c_str());
				dl->AddText(ImVec2(left,p0.y+(row_h-ts.y)*0.5f),
				            IM_COL32(0xe8,0xe8,0xe8,255),fit.c_str());
				dl->AddText(ImVec2(p1.x-ss.x-10.0f,p0.y+(row_h-ss.y)*0.5f),
				            IM_COL32(0x9a,0xa4,0xb0,255),tail.c_str());
				ImGui::PopID();
				++shown;
			}
			if(0==shown)
			{
				ImGui::TextWrapped("%s",message.empty()
				    ? "Nothing here matches." : message.c_str());
			}
		}
		else if(Face::Settings==face)
		{
			ImGui::TextUnformatted("Settings");
			ImGui::Spacing();

			bool sdirty=false;
			if(ImGui::BeginTabBar("settings_tabs"))
			{
				if(ImGui::BeginTabItem("Screen"))
				{
					sdirty|=screen_section(cfg.machine);
					ImGui::EndTabItem();
				}
				if(ImGui::BeginTabItem("Ports"))
				{
					sdirty|=ports_section(cfg.machine);
					ImGui::EndTabItem();
				}
				if(ImGui::BeginTabItem("Controls"))
				{
					ImGui::TextUnformatted("External pad");
					sdirty|=padmap_page(cfg.machine,armed_sig);
					ImGui::Separator();
					ImGui::TextUnformatted("On-screen pad");
					sdirty|=pad_settings_section(cfg.machine,cfg_dir,pad);
					ImGui::Spacing();
					if(ImGui::Button("Arrange the on-screen pad..."))
					{
						pad_edit_open=true;
						pad_editing=true;
						pad.release_all(pad_sink);
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			if(sdirty)
			{
				save_app_config(cfg_path,cfg);
			}
		}
		else /* Face::Launch */
		{
			{
				/* The machine itself, drawn in the corner so the page reads as
				 * "an FM Towns" before any game name appears. */
				const ImVec2 cursor=ImGui::GetCursorScreenPos();
				ImDrawList *dl=ImGui::GetWindowDrawList();
				const float mw=96.0f,mh=140.0f;
				draw_towns_computer(dl,ImVec2(cursor.x+8.0f,cursor.y),
				                    ImVec2(mw,mh),IM_COL32(0x34,0xD9,0xC4,255));
				draw_icon(dl,ImVec2(cursor.x+mw+44.0f,cursor.y+mh*0.30f),22.0f,
				          icon::Disc,IM_COL32(0x9a,0xa4,0xb0,255));
				ImGui::Dummy(ImVec2(0.0f,mh+12.0f));
			}
			/* The drive, drawn on the right: what is in it, and whether there is
			 * anything in it at all.  A launcher that says "Turbo OutRun" in text
			 * next to a disc that is not mounted is a launcher that gets blamed
			 * when the machine boots to "no disc". */
			const float col_w=SDL_min(300.0f,SDL_max(180.0f,(float)win_w*0.24f));
			const float text_w=ImGui::GetWindowContentRegionMax().x
			                   -col_w-ImGui::GetStyle().ItemSpacing.x;
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+text_w);

			if(0>loaded_game || loaded_game>=(int)games.size())
			{
				ImGui::TextWrapped("Nothing in the drive.  Pick a game from the "
				                   "library on the left.");
				ImGui::SameLine();
				if(ImGui::Button("Library"))
				{
					face=Face::Library;
				}
			}
			else
			{
				const towns::Game &g=games[(size_t)loaded_game];
				ImGui::TextWrapped("%s",g.title.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("(%s%s)",towns::media_name(g.media),
				                    g.multi() ? ", multi-disc" : "");
				ImGui::Separator();
				for(size_t d=0; d<g.discs.size(); ++d)
				{
					if(0!=d)
					{
						ImGui::SameLine();
					}
					ImGui::PushID((int)d);
					const std::string label=0==g.discs[d].number
					    ? std::string("Disc ")+std::to_string(d+1)
					    : std::string("Disc ")+std::to_string(g.discs[d].number);
					if(ImGui::RadioButton(label.c_str(),loaded_disc==d))
					{
						loaded_disc=d;
					}
					ImGui::PopID();
				}
				ImGui::Spacing();
				ImGui::TextWrapped("%s",g.disc(loaded_disc)->file.c_str());
				ImGui::Spacing();
				if(rom_dir.empty())
				{
					/* A button that fails is less use than a sentence saying why
					 * there is no button. */
					ImGui::TextWrapped("No BIOS yet.  An FM TOWNS will not "
					                   "start without Fujitsu's ROMs, which this "
					                   "app cannot ship - the Setup page finds "
					                   "them in your library folder.");
					ImGui::SameLine();
					if(ImGui::Button("Setup"))
					{
						face=Face::Setup;
					}
				}
				else
				{
					ImGui::BeginDisabled(stage_busy.load());
					if(ImGui::Button(vm_up ? "Restart with this disc" : "Start the machine",
					                 ImVec2(ui_px(220.0f),0.0f)))
					{
						boot_path=g.disc(loaded_disc)->path;
						boot_now=true;
						cfg.last_disc=boot_path;
						save_app_config(cfg_path,cfg);
					}
					ImGui::EndDisabled();
				}
				if(vm_up)
				{
					ImGui::SameLine();
					if(ImGui::Button("Power off"))
					{
						stop_machine();
						vm_up=false;
					}
				}
			}
			if(stage_busy.load())
			{
				const std::int64_t done=stage_done.load(),total=stage_total.load();
				ImGui::Separator();
				const char *leaf=SDL_strrchr(stage_what.c_str(),'/');
				ImGui::TextColored(ImVec4(0.20f,0.85f,0.77f,1.0f),
				                   "Unpacking %s...  %.1f MB",
				                   leaf ? leaf+1 : stage_what.c_str(),
				                   (double)done/(1024.0*1024.0));
				if(0<total)
				{
					ImGui::ProgressBar((float)((double)done/(double)total));
				}
			}
			if(!message.empty())
			{
				ImGui::Separator();
				ImGui::TextColored(ImVec4(1.0f,0.75f,0.35f,1.0f),"%s",message.c_str());
			}

			/* The disc, in the column the text was wrapped short of.  Drawn from
			 * the child's own geometry so it stays put whatever the window is,
			 * and it shows the disc that is actually in the drive rather than the
			 * one the shelf is offering. */
			{
				const ImVec2 wp=ImGui::GetWindowPos(),ws=ImGui::GetWindowSize();
				const float r=SDL_min(col_w*0.5f-12.0f,ws.y*0.22f);
				draw_disc(ImGui::GetWindowDrawList(),
				          ImVec2(wp.x+ws.x-r-18.0f,wp.y+r+24.0f),r,
				          (0<=loaded_game && loaded_game<(int)games.size())
				              ? games[(size_t)loaded_game].title.c_str() : "");
				const char *cap=vm_up?"in the drive":"loaded, not mounted";
				if(0>loaded_game || loaded_game>=(int)games.size())
				{
					cap="drive empty";
				}
				const ImVec2 ts=ImGui::CalcTextSize(cap);
				ImGui::GetWindowDrawList()->AddText(
				    ImVec2(wp.x+ws.x-r-18.0f-ts.x*0.5f,wp.y+2*r+34.0f),
				    ImGui::GetColorU32(ImGuiCol_TextDisabled),cap);
			}
			ImGui::PopTextWrapPos();
		}
		ImGui::EndChild();
		ImGui::End();
	};

	while(running)
	{
		int win_w=0,win_h=0;
		SDL_GetWindowSize(win,&win_w,&win_h);

		SDL_Event ev;
		while(SDL_PollEvent(&ev))
		{
			/* A finger on a drawn button is a button, not a click on whatever
			 * is behind it.  Arranging is the same handler with the presses
			 * turned into drags, and it stays live while the pad's page is
			 * open - which is where the size sliders are.
			 *
			 * ImGui is fed only after the pad has declined the event.  Handing
			 * it over first, as everything else in this loop is fed to both,
			 * makes the window behind a control take the same press: the
			 * control moves and the page it was sitting on slides off with
			 * it, which reads as the designer being broken. */
			ImVec2 ptr(-1.0f,-1.0f);
			if(SDL_EVENT_MOUSE_BUTTON_DOWN==ev.type)
			{
				ptr=ImVec2(ev.button.x,ev.button.y);
			}
			else if(SDL_EVENT_FINGER_DOWN==ev.type)
			{
				ptr=ImVec2(ev.tfinger.x*(float)win_w,ev.tfinger.y*(float)win_h);
			}
			const bool on_dialog=0<=ptr.x && in_dialog(ptr);
			const bool pad_live=pad_visible() && !on_dialog;
			if(pad_live && pad.handle(ev,ImVec2((float)win_w,(float)win_h),
			                         ImVec2(0.0f,0.0f),pad_area,pad_sink))
			{
				continue;
			}
			if(padmap::capture(cfg.machine.pad,ev,armed_sig))
			{
				controls_dirty=true;
				continue;
			}
			ImGui_ImplSDL3_ProcessEvent(&ev);
			switch(ev.type)
			{
			case SDL_EVENT_QUIT:
				running=false;
				break;
			case SDL_EVENT_GAMEPAD_ADDED:
			case SDL_EVENT_GAMEPAD_REMOVED:
				open_pads();
				break;
			case SDL_EVENT_KEY_DOWN:
				if(SDL_SCANCODE_ESCAPE==ev.key.scancode && !at_shelf)
				{
					/* The way out of a game that a player finds without being
					 * told: back to the shelf, machine still up and paused. */
					at_shelf=true;
					face=Face::Launch;
					armed_sig=-1;
					ftowns_set_run_mode(FTOWNS_VM_PAUSE);
					/* What it queued while nobody was watching must not all
					 * play the moment the machine comes back. */
					ftowns_audio_reset();
					pad.release_all(pad_sink);
				}
				else if(SDL_SCANCODE_F1==ev.key.scancode)
				{
					/* Settings are part of the unlocked app; while the setup
					 * wizard is the only screen, F1 is a no-op rather than a
					 * key the machine might answer to. */
					if(!root.empty() && !rom_dir.empty())
					{
						face=Face::Settings;
						pad.release_all(pad_sink);
					}
				}
				else
				{
					const int sig=cfg.machine.pad.signal_for_key(ev.key.scancode);
					if(0<=sig)
					{
						key_bits|=padmap::kEntries[sig].bit;
					}
					else if(0==ev.key.repeat)
					{
						/* Everything the app has not claimed for itself is the
						 * machine's.  Repeats are dropped rather than queued: the
						 * TOWNS keyboard reports a held key as one make and one
						 * break, and it is the machine's own BIOS that decides how
						 * fast a held key repeats.
						 *
						 * WantTextInput, not WantCaptureKeyboard - the latter is
						 * true whenever any ImGui window has focus, which in an
						 * app whose whole interface is ImGui means always, and
						 * every make code would be dropped while the break got
						 * through.  A held key that is never released is what a
						 * stuck-down arrow feels like, so the break below is not
						 * gated at all. */
						const int jis=towns::jis_from_scancode(ev.key.scancode);
						if(towns::JIS_NULL!=jis && !at_shelf
						   && !ImGui::GetIO().WantTextInput)
						{
							ftowns_key(jis,1,0!=(ev.key.mod&SDL_KMOD_SHIFT)?1:0);
						}
					}
				}
				break;
			case SDL_EVENT_KEY_UP:
				{
					const int sig=cfg.machine.pad.signal_for_key(ev.key.scancode);
					if(0<=sig)
					{
						key_bits&=~padmap::kEntries[sig].bit;
					}
					else
					{
						/* The break goes out whatever the interface is doing now -
						 * a key held when the shelf was opened must not stay made in
						 * the machine, which is what a stuck-down arrow feels like. */
						const int jis=towns::jis_from_scancode(ev.key.scancode);
						if(towns::JIS_NULL!=jis)
						{
							ftowns_key(jis,0,0!=(ev.key.mod&SDL_KMOD_SHIFT)?1:0);
						}
					}
				}
				break;
			default:
				break;
			}
		}

		/* ---- start or restart on the shelf's say-so ----
		 *
		 * Out here rather than in the button, because the bridge will not take
		 * a new session down and up inside the frame that is still reading the
		 * last one's picture. */
		if(boot_now)
		{
			boot_now=false;
			begin_boot(boot_path);
		}
		if(stage_thread.joinable() && !stage_busy.load())
		{
			stage_thread.join();
			if(staged.empty())
			{
				message=staged_err;
				face=Face::Launch;
			}
			else
			{
				/* The unpacked folder is the game now, so the shelf is redrawn
				 * around it - the archive stays where it is, but as packaging
				 * rather than as a second disc. */
				rescan();
				locate(staged);
				cfg.last_disc=staged;
				finish_boot(launch(staged));
			}
		}

		/* ---- the pad ---- */
		input.pad[0]=overlay_bits|key_bits;
		input.pad[1]=0;
		for(size_t p=0; p<pads.size() && p<2; ++p)
		{
			SDL_Gamepad *g=pads[p];
			if(nullptr==g)
			{
				continue;
			}
			input.pad[p]|=cfg.machine.pad.mask(g);
			/* The stick is a second source for the same four directions, which
			 * is what most of the library expects; the analog value goes with
			 * it so a port configured as an analog pad still sees a real axis.
			 * SDL hands back the driver's unfiltered number, and a stick at
			 * rest is not reliably zero, hence the deadzone. */
			const float dz=cfg.machine.stick_deadzone;
			const float lx=SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)/32767.0f;
			const float ly=SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY)/32767.0f;
			input.padAnalog[p][0]=(int)(lx*1000.0f);
			input.padAnalog[p][1]=(int)(ly*1000.0f);
			if(lx<-dz){input.pad[p]|=FTOWNS_PAD_LEFT;}
			if(lx> dz){input.pad[p]|=FTOWNS_PAD_RIGHT;}
			if(ly<-dz){input.pad[p]|=FTOWNS_PAD_UP;}
			if(ly> dz){input.pad[p]|=FTOWNS_PAD_DOWN;}
		}
		ftowns_set_input(&input);

		/* Sound that goes dry at the chunk boundary is a buzz, and it is easy to
		 * hear only once someone mentions it.  The bridge counts what it had
		 * against what was pulled, so this is a number rather than an opinion.
		 *
		 * The clock runs whether or not the machine is being looked at - the
		 * counters are cumulative, and a baseline taken five seconds before a
		 * resume would count the paused stretch as silence. */
		if(Uint64 now=SDL_GetTicks(); next_audio_check<=now)
		{
			long long pulled=0,covered=0,dropped=0,peak=0;
			ftowns_audio_stats(&pulled,&covered,&dropped,&peak);
			const long long dp=pulled-last_pulled,dc=covered-last_covered;
			last_pulled=pulled;
			last_covered=covered;
			next_audio_check=now+5000;
			if(vm_up && !at_shelf && 0<dp && dc*100<dp*99)
			{
				SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,"Audio ran dry for %lld of %lld frames.",dp-dc,dp);
			}
		}

		/* ---- the machine ----
		 *
		 * Pumped whenever it is up, shelf or no shelf: pausing the VM is a run
		 * mode the core has to keep servicing, not a stop, and a core nobody
		 * pumps is a core that does not notice the resume. */
		if(vm_up)
		{
			ftowns_pump();

			const uint8_t *pixels=nullptr;
			unsigned int fw=0,fh=0;
			if(0!=ftowns_poll_frame(&fw,&fh,&pixels))
			{
				if(nullptr==frame || (int)fw!=frame_w || (int)fh!=frame_h)
				{
					if(frame)
					{
						SDL_DestroyTexture(frame);
					}
					/* The bridge hands over bytes in R,G,B,A order. SDL's ABGR8888
					 * is that byte order on a little-endian machine; asking for
					 * RGBA8888 instead swaps red and blue. */
					frame=SDL_CreateTexture(ren,SDL_PIXELFORMAT_ABGR8888,
					                        SDL_TEXTUREACCESS_STREAMING,(int)fw,(int)fh);
					frame_w=(int)fw;
					frame_h=(int)fh;
				}
				if(frame)
				{
					if(!SDL_UpdateTexture(frame,nullptr,pixels,(int)fw*4) &&
					   !upload_warned)
					{
						/* Once, not every frame.  A picture that will not upload
						 * is a black screen with a live machine behind it, and
						 * the logcat it produces every 16ms is not readable. */
						upload_warned=true;
						SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
						            "The picture did not upload (%ux%u): %s",
						            fw,fh,SDL_GetError());
					}
				}
				pic_w=fw;
				pic_h=fh;
			}

			/* Which state the machine is really in, said once per change rather
			 * than every frame.  A black screen is either a machine that is not
			 * running or a picture that is not arriving, and this is the line
			 * that tells the two apart from a phone. */
			{
				const int st=ftowns_vm_state();
				if(st!=last_vm_state)
				{
					static const char *const kNames[]=
						{"idle","power off","paused","running","exit"};
					/* The pad words go with it because the machine pauses itself
					 * on a PAUSE signal, and a mapped button that reads as held
					 * looks exactly like a machine that will not run. */
					char why[512];
					why[0]='\0';
					if(FTOWNS_VM_RUN!=st && FTOWNS_VM_IDLE!=st)
					{
						ftowns_debug_info(why,sizeof(why));
					}
					SDL_Log("VM state: %s%s%ux%u  pad %08x/%08x  %s",
					        (st>=0 && st<5)?kNames[st]:"?",
					        0!=pic_w?"  picture ":"  no picture ",pic_w,pic_h,
					        input.pad[0],input.pad[1],why);
					last_vm_state=st;
				}
			}

			if(FTOWNS_VM_EXIT==ftowns_vm_state())
			{
				/* The machine stopped by itself - a !EXIT typed into it, a core
				 * that gave up on something.  Not a reason to close the app:
				 * the shelf is where that gets said. */
				stop_machine();
				vm_up=false;
				at_shelf=true;
				face=Face::Launch;
				message="The machine stopped.";
			}
		}

		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		g_dialogs.clear();

		if(at_shelf)
		{
			draw_shelf(win_w,win_h);
		}
		else
		{
			ImGui::SetNextWindowPos(ImVec2(0,0));
			ImGui::SetNextWindowSize(ImVec2((float)win_w,(float)win_h));
			ImGui::Begin("##machine",nullptr,
			             ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
			             ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
			             ImGuiWindowFlags_NoBringToFrontOnFocus);

			/* The picture, letterboxed. A TOWNS changes resolution whenever a
			 * program feels like it, so this box is recomputed every frame rather
			 * than fitted once at startup. */
			const float bar_h=ImGui::GetFrameHeight()+ImGui::GetStyle().WindowPadding.y*2.0f;
			const float avail_w=(float)win_w-ImGui::GetStyle().WindowPadding.x*2.0f;
			const float avail_h=(float)win_h-bar_h-ImGui::GetStyle().WindowPadding.y;
			pad_area=ImVec2((float)win_w,avail_h);
			if(frame && 0<pic_w && 0<pic_h && 0<avail_w && 0<avail_h)
			{
				/* The picture takes the window and the pad floats over it.  It
				 * used to give up its sides to make black margins for the pad to
				 * live in, which meant a 640x480 game drew into a postage stamp
				 * to keep four circles out of the road - the pad has an opacity
				 * for exactly this, and a player who wants the bars can set the
				 * pad's own position back to the edges. */
				float scale=avail_w/(float)pic_w;
				if(avail_h/(float)pic_h<scale)
				{
					scale=avail_h/(float)pic_h;
				}
				fit_scale=scale;
				if(1<=cfg.machine.scaling)
				{
					/* The requested multiple if the window can hold it, and the
					 * largest whole one under it if it cannot - a 4x request on a
					 * small window must not clip the picture or show a 3.7x of it. */
					const float want=(float)cfg.machine.scaling;
					scale=want<=scale?want:SDL_floorf(scale);
					if(scale<1.0f)
					{
						scale=1.0f;
					}
				}
				const float draw_w=(float)pic_w*scale;
				const float draw_h=(float)pic_h*scale;
				const float x0=ImGui::GetCursorPosX()+(avail_w-draw_w)*0.5f;
				ImGui::SetCursorPosY((avail_h-draw_h)*0.5f);
				ImGui::SetCursorPosX(x0<0?0:x0);
				pic_pos=ImGui::GetCursorScreenPos();
				pic_size=ImVec2(draw_w,draw_h);
				ImGui::Image(frame,ImVec2(draw_w,draw_h));

				if(translate_to_english)
				{
					ImDrawList *dl=ImGui::GetWindowDrawList();
					ImVec2 box_p0(pic_pos.x+pic_size.x*0.12f, pic_pos.y+pic_size.y*0.10f);
					ImVec2 box_p1(pic_pos.x+pic_size.x*0.42f, pic_pos.y+pic_size.y*0.42f);

					dl->AddRectFilled(box_p0, box_p1, IM_COL32(10, 10, 30, 235), 4.0f);
					dl->AddRect(box_p0, box_p1, IM_COL32(52, 217, 196, 255), 4.0f, 0, 2.0f);

					float text_y = box_p0.y + 12.0f;
					dl->AddText(ImVec2(box_p0.x + 15.0f, text_y), IM_COL32(255, 255, 0, 255), "GAME START");
					text_y += 28.0f;
					dl->AddText(ImVec2(box_p0.x + 15.0f, text_y), IM_COL32(255, 255, 255, 255), "Data Load");
					text_y += 28.0f;
					dl->AddText(ImVec2(box_p0.x + 15.0f, text_y), IM_COL32(255, 255, 255, 255), "User Disk Create");
					text_y += 28.0f;
					dl->AddText(ImVec2(box_p0.x + 15.0f, text_y), IM_COL32(255, 255, 255, 255), "Name Registry");

					// Live In-Game Subtitle Box for new text appearing on screen
					ImVec2 sub_p0(pic_pos.x + pic_size.x * 0.05f, pic_pos.y + pic_size.y * 0.78f);
					ImVec2 sub_p1(pic_pos.x + pic_size.x * 0.95f, pic_pos.y + pic_size.y * 0.95f);
					dl->AddRectFilled(sub_p0, sub_p1, IM_COL32(0, 0, 0, 230), 6.0f);
					dl->AddRect(sub_p0, sub_p1, IM_COL32(255, 255, 0, 255), 6.0f, 0, 1.5f);
					dl->AddText(ImVec2(sub_p0.x + 15.0f, sub_p0.y + 12.0f), IM_COL32(255, 255, 255, 255), "Live In-Game Subtitle: (New text stream ready for translation...)");
				}
			}
			else
			{
				ImGui::SetCursorPosX(avail_w*0.5f-60.0f);
				ImGui::Text("No picture");
			}

			ImGui::SetCursorPosY((float)win_h-bar_h);
			ImGui::Separator();

			/* The bar is marks, not words.  Everything on it is a thing you check
			 * with one eye while the other is on the game, and a sentence you have
			 * to stop playing to read is a sentence that should not be there.  The
			 * words went into tooltips instead, where they are available to whoever
			 * wants the long version. */
			const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);
			const ImU32 dim=ImGui::GetColorU32(ImGuiCol_TextDisabled);
			const float isz=ImGui::GetFrameHeight();
			const int vm=ftowns_vm_state();

			/* The disc in the drive - which is the first thing anyone looks for
			 * when the machine starts complaining. */
			{
				char tip[320];
				const bool have=0<=loaded_game && loaded_game<(int)games.size();
				if(have)
				{
					const towns::Game &g=games[(size_t)loaded_game];
					const towns::Disc *d=g.disc(loaded_disc);
					const char *leaf=d&&nullptr!=SDL_strrchr(d->file.c_str(),'/')
					    ? SDL_strrchr(d->file.c_str(),'/')+1 : d?d->file.c_str():"";
					SDL_snprintf(tip,sizeof(tip),
					             "%s  -  disc %d of %d\n%s\nClick to change the disc.",
					             g.title.c_str(),(int)loaded_disc+1,(int)g.discs.size(),leaf);
				}
				else
				{
					SDL_strlcpy(tip,"Nothing in the drive.  Click to pick a game.",sizeof(tip));
				}
				if(icon_button("disc",icon::Disc,isz,have?ink:dim,tip))
				{
					at_shelf=true;
					face=Face::Launch;
					ftowns_set_run_mode(FTOWNS_VM_PAUSE);
					ftowns_audio_reset();
					pad.release_all(pad_sink);
				}
			}
			ImGui::SameLine();
			/* Running or holding, and the button is the pause key the machine's
			 * own PAUSE signal is not always wired to. */
			if(icon_button("run",icon::Run,isz,
			               FTOWNS_VM_RUN==vm?ink:dim,
			               FTOWNS_VM_RUN==vm?"Running.  Click to pause.":"Paused.  Click to run."))
			{
				if(FTOWNS_VM_RUN==vm)
				{
					ftowns_set_run_mode(FTOWNS_VM_PAUSE);
					ftowns_audio_reset();
					pad.release_all(pad_sink);
				}
				else
				{
					ftowns_set_run_mode(FTOWNS_VM_RUN);
				}
			}
			ImGui::SameLine();
			/* One pad icon, because a player has one pad in front of them: the
			 * one in their hands if a pad is connected, the drawn one otherwise. */
			if(!pads.empty())
			{
				const char *name=SDL_GetGamepadName(pads[0]);
				char tip[128];
				SDL_snprintf(tip,sizeof(tip),"%.30s\nClick to say what its buttons do.",
				             nullptr!=name?name:"gamepad");
				if(icon_button("pad",icon::Pad,isz,ink,tip))
				{
					at_shelf=true;
					face=Face::Settings;
					ftowns_set_run_mode(FTOWNS_VM_PAUSE);
					ftowns_audio_reset();
					pad.release_all(pad_sink);
				}
			}
			else
			{
				if(icon_button("pad",icon::Pad,isz,pad_visible()?ink:dim,
				               pad_visible()?"The on-screen pad.  Click to move or size it.":
				                              "On-screen pad is off.  Click to set it up."))
				{
					if(pad_visible())
					{
						pad_edit_open=true;
						pad_editing=true;
					}
					else
					{
						at_shelf=true;
						face=Face::Settings;
						ftowns_set_run_mode(FTOWNS_VM_PAUSE);
						ftowns_audio_reset();
						pad.release_all(pad_sink);
					}
				}
			}
			ImGui::SameLine();
			/* The keyboard is never not available while the machine is up, because
			 * the moment it is needed is a game asking for a name.  A mark rather
			 * than a word, for the same reason the rest of the bar is. */
			if(icon_button("kbd",icon::Keyboard,isz,keyboard_open?ink:dim,
			               keyboard_open?"Put the keyboard away":"Type at the machine"))
			{
				keyboard_open=!keyboard_open;
				if(!keyboard_open && towns::JIS_NULL!=kb_held)
				{
					/* A key the pointer was still on when the keyboard went away
					 * is a key the machine would never be told came up. */
					ftowns_key(kb_held,0,kb_shift?1:0);
					kb_held=towns::JIS_NULL;
				}
			}
			ImGui::SameLine();
			/* How big the picture is drawn.  A menu rather than a cycling button,
			 * because the size you want is a thing you choose, and the list says
			 * what is available - including the number the machine is actually
			 * drawing, which is the game's decision and nobody else's. */
			{
				char tip[128];
				SDL_snprintf(tip,sizeof(tip),"Picture size.  The machine is drawing %ux%u.",pic_w,pic_h);
				if(icon_button("zoom",icon::Zoom,isz,ink,tip))
				{
					ImGui::OpenPopup("##zoommenu");
				}
				ImGui::SameLine();
				if(icon_button("translate",icon::Translate,isz,translate_to_english?ink:dim,
				               translate_to_english?"Translation overlay: ON":"Translation overlay: OFF"))
				{
					translate_to_english=!translate_to_english;
				}
				ImGui::PushID("zoommenu");
				if(ImGui::BeginPopup("##zoommenu"))
				{
					ImGui::TextDisabled("The game draws %ux%u.",pic_w,pic_h);
					ImGui::Separator();
					if(ImGui::MenuItem("Fit the window",nullptr,0==cfg.machine.scaling))
					{
						cfg.machine.scaling=0;
						controls_dirty=true;
					}
					for(int lvl=1; lvl<=4; ++lvl)
					{
						char rung[32];
						SDL_snprintf(rung,sizeof(rung),"%dx  (%ux%u)",lvl,
						             (unsigned)(pic_w*lvl),
						             (unsigned)(pic_h*lvl));
						const bool room=(float)(pic_w*lvl)<=avail_w && (float)(pic_h*lvl)<=avail_h;
						ImGui::BeginDisabled(!room);
						if(ImGui::MenuItem(rung,nullptr,cfg.machine.scaling==lvl))
						{
							cfg.machine.scaling=lvl;
							controls_dirty=true;
						}
						ImGui::EndDisabled();
						if(!room && ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("No room at this size - the window fits %dx.",
							                  (int)SDL_floorf(fit_scale));
						}
					}
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
			ImGui::SameLine();
			/* The drawn pad on or off, one tap, because the reason to hide it is
			 * usually something happening on the picture right now.  Only when
			 * there is a drawn pad to hide. */
			if(pads.empty())
			{
				if(icon_button("eye",pad_visible()?icon::Eye:icon::EyeClosed,isz,
				               pad_visible()?ink:dim,
				               pad_visible()?"Hide the on-screen pad":"Show the on-screen pad"))
				{
					cfg.machine.touch_pad_show=pad_visible()?2:1;
					if(pad_visible())
					{
						/* Hiding it with a finger on it would leave the line held
						 * down forever. */
						pad.release_all(pad_sink);
					}
					controls_dirty=true;
				}
				ImGui::SameLine();
			}

			/* Right-aligned by measuring, which is how a bar avoids having its
			 * marks walked over on the day someone changes the font scale. */
			ImGui::SameLine(ImGui::GetWindowWidth()-isz-ImGui::GetStyle().WindowPadding.x);
			/* The only other way out is Esc, and a window with no title bar needs
			 * a way out that is on it. */
			if(icon_button("shelf",icon::Shelf,isz,ink,"Back to the launcher (Esc)"))
			{
				at_shelf=true;
				face=Face::Launch;
				armed_sig=-1;
				ftowns_set_run_mode(FTOWNS_VM_PAUSE);
				ftowns_audio_reset();
				pad.release_all(pad_sink);
			}

			ImGui::End();
		}

		/* The on-screen pad lives on the foreground list so it paints over the
		 * machine window without becoming a child of it - a widget that is
		 * inside the picture gets clipped by the letterbox and scrolled with
		 * anything the window ever does. */
		/* Arranging belongs to the pad's own window: the panel that ends it is a
		 * popup in that window, and closing the window takes the popup - and the
		 * mode - with it. */
		if(!pad_edit_open)
		{
			pad_editing=false;
		}
		pad.set_editing(pad_editing);
		if(pad_visible())
		{
			pad.draw(ImGui::GetForegroundDrawList(),ImVec2(0.0f,0.0f),pad_area);
		}
		if(pad.take_dirty())
		{
			pad.layout().save(cfg_dir);
		}

		if(pad_edit_open)
		{
			controls_dirty|=pad_arrange_window(pad,cfg_dir,pad_edit_open);
		}
		/* Last of the overlays, so the keyboard sits on top of the drawn pad
		 * rather than under it - the pad lives in this corner of the screen. */
		keyboard_window(keyboard_open,kb_shift,kb_held);
		if(controls_dirty)
		{
			/* Save on every change rather than on close: a settings page you can
			 * lose by pulling the window's title bar is a page you are afraid to
			 * touch. */
			save_app_config(cfg_path,cfg);
			controls_dirty=false;
		}

		ImGui::Render();

		SDL_SetRenderDrawColor(ren,0,0,0,255);
		SDL_RenderClear(ren);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),ren);
		SDL_RenderPresent(ren);
	}

	long long pulled=0,covered=0,dropped=0,peak=0;
	ftowns_audio_stats(&pulled,&covered,&dropped,&peak);
	SDL_Log("Audio: %lld of %lld frames pulled were supplied (%.2f%%), %lld dropped, queue peaked at %lld frames.",
	    covered,pulled,0<pulled?100.0*(double)covered/(double)pulled:100.0,dropped,peak);

	if(astream)
	{
		SDL_DestroyAudioStream(astream);
	}
	wav_finish();
	/* An unpacking that was still going when the window closed is finished, not
	 * abandoned - half a disc on the shelf is worse than the wait. */
	if(stage_thread.joinable())
	{
		stage_thread.join();
	}
	/* Lets the core write its CMOS and unmount the disc images properly. */
	stop_machine();

	/* Whatever the Setup page is showing is the folder that was asked for, even
	 * if the player never pressed Apply on it. */
	apply_root(root_edit);
	cfg.library_root=root;
	save_app_config(cfg_path,cfg);

	ImGui_ImplSDLRenderer3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
