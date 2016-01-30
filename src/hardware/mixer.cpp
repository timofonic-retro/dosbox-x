/*
 *  Copyright (C) 2002-2015  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


/* 
	Remove the sdl code from here and have it handeld in the sdlmain.
	That should call the mixer start from there or something.
*/

#include <string.h>
#include <sys/types.h>
#include <math.h>

#if defined (WIN32)
//Midi listing
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include "SDL.h"
#include "mem.h"
#include "pic.h"
#include "dosbox.h"
#include "mixer.h"
#include "timer.h"
#include "setup.h"
#include "cross.h"
#include "support.h"
#include "control.h"
#include "mapper.h"
#include "hardware.h"
#include "programs.h"

// NTS: This is not used here. This is defined for OTHER cpp files to render audio into
//      before submitting it to us.
Bit8u MixTemp[MIXER_BUFSIZE];

#define MIXER_SSIZE 4
#define MIXER_SHIFT 14
#define MIXER_REMAIN ((1<<MIXER_SHIFT)-1)
#define MIXER_VOLSHIFT 13

static INLINE Bit16s MIXER_CLIP(Bits SAMP) {
	if (SAMP < MAX_AUDIO) {
		if (SAMP > MIN_AUDIO)
			return SAMP;
		else
			return MIN_AUDIO;
	}

	return MAX_AUDIO;
}

static struct {
	// NTS: This is a circular buffer that the mixer code uses to hand off
	//      audio to the SDL audio callback. You must use SDL audio lock/unlock
	//      to safely modify the read/write variables;
	Bit32s		output_buffer[MIXER_BUFSIZE][2];
	Bitu		output_buffer_write, output_buffer_read;	// SDL callback reads, main thread writes
	Bitu		min_needed, max_needed;
	float		mastervol[2];
	MixerChannel*	channels;
	bool		nosound;
	Bit32u		freq;
	Bit32u		freq_inv16b;			// (1 << 16) / freq for interpolation
	Bit32u		blocksize;
	bool		swapstereo;
	bool		sampleaccurate;

	Bitu		samples_per_ms;			// whole samples per millisecond
	Bitu		samples_per_ms_f;		// fractional samples per millisecond (1/1000th of sample)
	Bitu		current_sample_f_count;		// fractional accumulator of samples
	Bitu		current_sample_per_ms;		// current number of samples to render in this ms
	Bitu		current_second_sample_counter;	// number of samples rendered within the second
	Bitu		current_ms;			// count of milliseconds to the next whole second
} mixer;

bool Mixer_SampleAccurate() {
	return mixer.sampleaccurate;
}

MixerChannel * MIXER_AddChannel(MIXER_Handler handler,Bitu freq,const char * name) {
	MixerChannel * chan=new MixerChannel();
	chan->allow_overrendering = false;
	chan->underrun_wait = 0;
	chan->src_rate_f = 0;
	chan->scale = 1.0;
	chan->render_pos = 0;
	chan->render_max = 0;
	chan->handler=handler;
	chan->name=name;
	chan->SetFreq(freq);
	chan->next=mixer.channels;
	chan->SetVolume(1,1);
	chan->enabled=false;
	chan->delta_rem = -((Bits)mixer.freq);
	chan->last[0] = chan->last[1] = 0;
	chan->current[0] = chan->current[1] = 0;
	mixer.channels=chan;
	return chan;
}

MixerChannel * MIXER_FindChannel(const char * name) {
	MixerChannel * chan=mixer.channels;
	while (chan) {
		if (!strcasecmp(chan->name,name)) break;
		chan=chan->next;
	}
	return chan;
}

void MIXER_DelChannel(MixerChannel* delchan) {
	MixerChannel * chan=mixer.channels;
	MixerChannel * * where=&mixer.channels;
	while (chan) {
		if (chan==delchan) {
			*where=chan->next;
			delete delchan;
			return;
		}
		where=&chan->next;
		chan=chan->next;
	}
}

void MixerChannel::UpdateVolume(void) {
	volmul[0]=(Bits)((1 << MIXER_VOLSHIFT)*scale*volmain[0]*mixer.mastervol[0]);
	volmul[1]=(Bits)((1 << MIXER_VOLSHIFT)*scale*volmain[1]*mixer.mastervol[1]);
}

void MixerChannel::SetVolume(float _left,float _right) {
	volmain[0]=_left;
	volmain[1]=_right;
	UpdateVolume();
}

void MixerChannel::SetScale( float f ) {
	scale = f;
	UpdateVolume();
}

void MixerChannel::Enable(bool _yesno) {
	if (_yesno==enabled) return;
	enabled=_yesno;
}

void MixerChannel::SetFreq(Bitu _freq) {
	src_rate = _freq;
}

void MixerChannel::RemoveRenderedSamples(Bitu count) {
	if (count > render_pos) count = render_pos;

	if (count != 0) {
		Bitu rem = render_pos - count;
		if (rem != 0) memmove(&render[0][0],&render[count][0],rem*sizeof(render[0]));
		render_pos = rem;
	}
	else {
		render_pos = 0;
	}
}

void MixerChannel::Mix(Bitu _needed) {
	Bitu count;

	done = 0;
	needed = _needed;
	count = (needed * src_rate) + src_rate_f;
	src_rate_f = count % mixer.freq;
	count /= mixer.freq;
	if (count > 0) handler(count);
}

void MixerChannel::AddSilence(void) {
}

template<class Type,bool stereo,bool signeddata,bool nativeorder>
inline void MixerChannel::LoadSample(Bitu &len, const Type* &data) {
	if (len > 0) {
		last[0] = current[0];
		last[1] = current[1];

		if (sizeof(Type) == 1) {
			if (!signeddata) {
				if (stereo) {
					current[0] = ((Bit8s)(*data++ ^ 0x80)) << 8;
					current[1] = ((Bit8s)(*data++ ^ 0x80)) << 8;
				}
				else {
					current[0] = ((Bit8s)(*data++ ^ 0x80)) << 8;
				}
			}
			else {
				if (stereo) {
					current[0] = ((Bit8s)(*data++)) << 8;
					current[1] = ((Bit8s)(*data++)) << 8;
				}
				else {
					current[0] = ((Bit8s)(*data++)) << 8;
				}
			}
		}
		else {
			//16bit and 32bit both contain 16bit data internally
			if (signeddata) {
				if (stereo) {
					if (nativeorder) {
						current[0] = *data++;
						current[1] = *data++;
					}
					else {
						if (sizeof(Type) == 2) {
							current[0] = (Bit16s)host_readw((HostPt)(data++));
							current[1] = (Bit16s)host_readw((HostPt)(data++));
						}
						else {
							current[0] = (Bit32s)host_readd((HostPt)(data++));
							current[1] = (Bit32s)host_readd((HostPt)(data++));
						}
					}
				}
				else {
					if (nativeorder) {
						current[0] = *data++;
					}
					else {
						if (sizeof(Type) == 2) {
							current[0] = (Bit16s)host_readw((HostPt)(data++));
						}
						else {
							current[0] = (Bit32s)host_readd((HostPt)(data++));
						}
					}
				}
			}
			else {
				if (stereo) {
					if (nativeorder) {
						current[0] = (Bit16s)((*data++) ^ 0x8000);
						current[1] = (Bit16s)((*data++) ^ 0x8000);
					}
					else {
						if (sizeof(Type) == 2) {
							current[0] = (Bit16s)(host_readw((HostPt)(data++)) ^ 0x8000);
							current[1] = (Bit16s)(host_readw((HostPt)(data++)) ^ 0x8000);
						}
						else {
							current[0] = (Bit32s)(host_readd((HostPt)(data++)) ^ 0x80000000);
							current[1] = (Bit32s)(host_readd((HostPt)(data++)) ^ 0x80000000);
						}
					}
				}
				else {
					if (nativeorder) {
						current[0] = (Bit16s)((*data++) ^ 0x8000);
					}
					else {
						if (sizeof(Type) == 2) {
							current[0] = (Bit16s)(host_readw((HostPt)(data++)) ^ 0x8000);
						}
						else {
							current[0] = (Bit32s)(host_readd((HostPt)(data++)) ^ 0x80000000);
						}
					}
				}
			}
		}

		current[0] *= volmul[0];
		if (stereo) current[1] *= volmul[1];
		else current[1] = current[0];
	
		delta_rem += mixer.freq;
		len--;
	}
}

inline void MixerChannel::RenderSample() {
	if (render_pos < render_max) {
		Bit64s m = (mixer.freq - delta_rem) * mixer.freq_inv16b;

		render[render_pos][0] = last[0] + (((current[0] - last[0]) * (Bit64s)m) >> (Bit64s)16);
		render[render_pos][1] = last[1] + (((current[1] - last[1]) * (Bit64s)m) >> (Bit64s)16);
		delta_rem -= src_rate;
		render_pos++;
		done++;
	}
}

template<class Type,bool stereo,bool signeddata,bool nativeorder>
inline void MixerChannel::AddSamples(Bitu len, const Type* data) {
	for (;;) {
		if (delta_rem >= (Bits)0) {
			if (render_pos >= render_max) {
				if (len > 0)
					LOG_MSG("AddSamples %u unrendered source samples due to render overrun in '%s' %u > %u",
						(unsigned int)len,name,(unsigned int)render_pos,(unsigned int)render_max);

				break;
			}

			RenderSample();
		}
		else {
			if (len == 0) break;
			LoadSample<Type,stereo,signeddata,nativeorder>(len,data);
		}
	}
}

void MixerChannel::AddStretched(Bitu len,Bit16s * data) {
}

void MixerChannel::AddStretchedStereo(Bitu len/*combined L+R samples*/,Bit16s * data) {
}

void MixerChannel::AddSamples_m8(Bitu len, const Bit8u * data) {
	AddSamples<Bit8u,false,false,true>(len,data);
}
void MixerChannel::AddSamples_s8(Bitu len,const Bit8u * data) {
	AddSamples<Bit8u,true,false,true>(len,data);
}
void MixerChannel::AddSamples_m8s(Bitu len,const Bit8s * data) {
	AddSamples<Bit8s,false,true,true>(len,data);
}
void MixerChannel::AddSamples_s8s(Bitu len,const Bit8s * data) {
	AddSamples<Bit8s,true,true,true>(len,data);
}
void MixerChannel::AddSamples_m16(Bitu len,const Bit16s * data) {
	AddSamples<Bit16s,false,true,true>(len,data);
}
void MixerChannel::AddSamples_s16(Bitu len,const Bit16s * data) {
	AddSamples<Bit16s,true,true,true>(len,data);
}
void MixerChannel::AddSamples_m16u(Bitu len,const Bit16u * data) {
	AddSamples<Bit16u,false,false,true>(len,data);
}
void MixerChannel::AddSamples_s16u(Bitu len,const Bit16u * data) {
	AddSamples<Bit16u,true,false,true>(len,data);
}
void MixerChannel::AddSamples_m32(Bitu len,const Bit32s * data) {
	AddSamples<Bit32s,false,true,true>(len,data);
}
void MixerChannel::AddSamples_s32(Bitu len,const Bit32s * data) {
	AddSamples<Bit32s,true,true,true>(len,data);
}
void MixerChannel::AddSamples_m16_nonnative(Bitu len,const Bit16s * data) {
	AddSamples<Bit16s,false,true,false>(len,data);
}
void MixerChannel::AddSamples_s16_nonnative(Bitu len,const Bit16s * data) {
	AddSamples<Bit16s,true,true,false>(len,data);
}
void MixerChannel::AddSamples_m16u_nonnative(Bitu len,const Bit16u * data) {
	AddSamples<Bit16u,false,false,false>(len,data);
}
void MixerChannel::AddSamples_s16u_nonnative(Bitu len,const Bit16u * data) {
	AddSamples<Bit16u,true,false,false>(len,data);
}
void MixerChannel::AddSamples_m32_nonnative(Bitu len,const Bit32s * data) {
	AddSamples<Bit32s,false,true,false>(len,data);
}
void MixerChannel::AddSamples_s32_nonnative(Bitu len,const Bit32s * data) {
	AddSamples<Bit32s,true,true,false>(len,data);
}

void MixerChannel::FillUp(void) {
	SDL_LockAudio();
#if 0
	if (!enabled || done<mixer.done) {
		SDL_UnlockAudio();
		return;
	}
	float index=PIC_TickIndex();
	Mix((Bitu)(index*mixer.needed));
#endif
	SDL_UnlockAudio();
}

extern bool ticksLocked;
static inline bool Mixer_irq_important(void) {
	/* In some states correct timing of the irqs is more important then 
	 * non stuttering audo */
	return (ticksLocked || (CaptureState & (CAPTURE_WAVE|CAPTURE_VIDEO)));
}

static Bit32s MIXER_RenderOutTmp[4096][2];

static void MIXER_RenderOut(Bitu needed) {
	MixerChannel * chan=mixer.channels;
	Bitu o,count,render,nrender;

	memset(MIXER_RenderOutTmp,0,sizeof(MIXER_RenderOutTmp));

	while (chan) {
		count = 0;
		if (chan->enabled) {
			render = needed;
			if (render >= chan->underrun_wait) {
				render -= chan->underrun_wait;
				chan->underrun_wait = 0;
			}
			else {
				if (chan->underrun_wait >= render)
					chan->underrun_wait -= render;
				else
					chan->underrun_wait = 0;

				render = 0;
			}
			nrender = render;
			if (render > chan->render_pos) render = chan->render_pos;

			for (count=0;count < render;count++) {
				MIXER_RenderOutTmp[count][0] += chan->render[count][0];
				MIXER_RenderOutTmp[count][1] += chan->render[count][1];
			}

			if (count != 0 && count < nrender) {
				chan->underrun_wait += (nrender-count);
				if (chan->underrun_wait > needed) chan->underrun_wait = needed;
				fprintf(stderr,"WARNING, channel '%s' render underrun %u < %u\n",
					chan->name,
					(unsigned int)count,
					(unsigned int)nrender);
			}
		}
		chan->RemoveRenderedSamples(count);
		chan=chan->next;
	}

	if (CaptureState & (CAPTURE_WAVE|CAPTURE_VIDEO)) {
		Bit16s convert[1024][2];
	
		for (Bitu i=0;i < needed;i++) {
			convert[i][0]=MIXER_CLIP(MIXER_RenderOutTmp[i][0] >> MIXER_VOLSHIFT);
			convert[i][1]=MIXER_CLIP(MIXER_RenderOutTmp[i][1] >> MIXER_VOLSHIFT);
		}
		CAPTURE_AddWave(mixer.freq, needed, (Bit16s*)convert);
	}

	count = 0;
	o = mixer.output_buffer_write;
	do {
		if ((++o) == MIXER_BUFSIZE) o = 0;
		if (o == mixer.output_buffer_read) break;
		mixer.output_buffer[mixer.output_buffer_write][0] = MIXER_RenderOutTmp[count][0];
		mixer.output_buffer[mixer.output_buffer_write][1] = MIXER_RenderOutTmp[count][0];
		mixer.output_buffer_write = o;
	} while ((++count) < needed);
}

/* Mix a certain amount of new samples */
static void MIXER_MixData(Bitu needed) {
	MixerChannel * chan=mixer.channels;
	while (chan) {
		if (chan->allow_overrendering)
			chan->render_max = MIXER_RENDERBUFSIZE;
		else
			chan->render_max = mixer.current_sample_per_ms;

		chan->Mix(needed);
		chan=chan->next;
	}
}

unsigned long long mixer_sample_counter = 0;
double mixer_start_pic_time = 0;

/* you'd be surprised how off-time the mixer can get following the sound card vs the emulation */
static int MIXER_CheckTimeSync() {
	if( Mixer_irq_important() ) {
		double time = PIC_FullIndex() - mixer_start_pic_time;
		double should = ((double)mixer_sample_counter * 1000) / mixer.freq;
		double error = should - time;
		double adj = (error < 0 ? -1 : 1) * error * error * 0.075;
		if (adj < -20) adj = -20;
		else if (adj > 20) adj = 20;
		int iadj = (int)((-adj * mixer.freq * (1 << MIXER_SHIFT)) / 1000);
//		LOG_MSG("iadj=%d %.6f",iadj,(double)iadj / (1 << MIXER_SHIFT));
		return iadj;
	}
	else {
		mixer_start_pic_time = PIC_FullIndex();
		mixer_sample_counter = 0;
	}

	return 0;
}

static void MIXER_NextMillisecond() {
	// start of second reset, when moving from 0ms -> 1ms
	if (mixer.current_ms == 0) {
		mixer.current_second_sample_counter = 0;
		mixer.current_sample_f_count = 0;
	}

	// at the time of this call, the number of samples we indicated were rendered
	mixer.current_second_sample_counter += mixer.current_sample_per_ms;

	// now compute the number of samples we render next
	mixer.current_ms++;
	mixer.current_sample_per_ms = mixer.samples_per_ms;
	mixer.current_sample_f_count += mixer.samples_per_ms_f;
	if (mixer.current_sample_f_count >= 1000) {
		// if the fractional part adds up to >= 1 sec, then carry the 1 second excess from the fraction to the whole.
		// we're rendering an extra sample this time around.
		mixer.current_sample_f_count -= 1000;
		mixer.current_sample_per_ms++;
	}

	if (mixer.current_ms >= 1000) { // one second has elapsed
		if (mixer.current_ms > 1000)
			LOG_MSG("MIXER warning: current_ms > 1000, what happened?");
		if (mixer.current_sample_f_count >= 1000)
			LOG_MSG("MIXER warning: current_sample_f_count >= 1000, what happened?");

		// the number of samples rendered in the second should equal the sample rate. does it?
		if (mixer.current_second_sample_counter != mixer.freq)
			LOG_MSG("MIXER warning: too many/few samples rendered in the last second (rendered=%u expected=%u)",
				(unsigned int)mixer.current_second_sample_counter,
				(unsigned int)mixer.freq);

		// the fractional part was computed as freq % 1000, therefore should be zero by now. is it?
		if (mixer.current_sample_f_count != 0)
			LOG_MSG("MIXER warning: one second elapsed, fractional part nonzero, loss of accuracy over time may occur");

		// one second elaped. reset.
		mixer.current_ms = 0;
	}
}

static void MIXER_NextSample() {
	// TODO: Count samples up to millisecond sample count then call next millisecond
}

static void MIXER_MixPICEvent(Bitu val) {
	// FIXME: This code needs to render one sample then count samples up to the 1ms sample count computed, then call Next Millisecond
#if 0
	int tick_adj = MIXER_CheckTimeSync();

	SDL_LockAudio();
	if (mixer.needed > 0) {
		mixer_sample_counter += mixer.needed-mixer.done;
		MIXER_MixData(mixer.needed);
	}
	mixer.tick_remain+=mixer.tick_add+tick_adj;
	if ((Bit32s)mixer.tick_remain < 0) mixer.tick_remain = 0;
	mixer.needed+=(mixer.tick_remain>>MIXER_SHIFT);
	mixer.tick_remain&=MIXER_REMAIN;
	SDL_UnlockAudio();

#endif
	MIXER_NextSample();
	PIC_AddEvent(MIXER_MixPICEvent,1000.0 / mixer.freq,0);
}

static void MIXER_Mix(void) {
	MIXER_MixData(mixer.current_sample_per_ms);
	SDL_LockAudio();
	MIXER_RenderOut(mixer.current_sample_per_ms);
	SDL_UnlockAudio();
	MIXER_NextMillisecond();
}

static void MIXER_Mix_NoSound(void) {
#if 0
	if (mixer.needed > 0) {
		mixer_sample_counter += mixer.needed-mixer.done;
		MIXER_MixData(mixer.needed);
	}
	/* Clear piece we've just generated */
	for (Bitu i=0;i<mixer.needed;i++) {
		mixer.work[mixer.pos][0]=0;
		mixer.work[mixer.pos][1]=0;
		mixer.pos=(mixer.pos+1)&MIXER_BUFMASK;
	}
	/* Reduce count in channels */
	for (MixerChannel * chan=mixer.channels;chan;chan=chan->next) {
		if (chan->done>mixer.needed) chan->done-=mixer.needed;
		else chan->done=0;
	}
	/* Set values for next tick */
	mixer.tick_remain+=mixer.tick_add;
	if ((Bit32s)mixer.tick_remain < 0) mixer.tick_remain = 0;
	mixer.needed=mixer.tick_remain>>MIXER_SHIFT;
	mixer.tick_remain&=MIXER_REMAIN;
	mixer.done=0;
#endif
	MIXER_NextMillisecond();
}

static Bitu MIXER_GetSequentialOutputBufferAvail() {
	if (mixer.output_buffer_read <= mixer.output_buffer_write)
		return mixer.output_buffer_write - mixer.output_buffer_read;
	
	return MIXER_BUFSIZE - mixer.output_buffer_read;
}

static Bitu MIXER_GetTotalOutputBufferAvail() {
	if (mixer.output_buffer_read <= mixer.output_buffer_write)
		return mixer.output_buffer_write - mixer.output_buffer_read;
	
	return MIXER_BUFSIZE + mixer.output_buffer_write - mixer.output_buffer_read; /* write < read */
}

static void MIXER_SDL_CallBack(void * userdata, Uint8 *stream, int len) {
	Bit16s *output=(Bit16s *)stream;
	Bitu need=(Bitu)len / MIXER_SSIZE;
	Bitu src_avail,total_src_avail;
	unsigned int ifrac=0,istep=1 << MIXER_SHIFT;
	Bit32s *input;

	if (need == 0) return;

	// initial assessment of the buffer fullness, so we can stretch to fill output
	total_src_avail = MIXER_GetTotalOutputBufferAvail();
	if (total_src_avail != 0) {
		if (total_src_avail < mixer.min_needed)
			istep = (total_src_avail << MIXER_SHIFT) / mixer.min_needed;
		else if (total_src_avail > mixer.max_needed)
			istep = (total_src_avail << MIXER_SHIFT) / mixer.max_needed;

		while (need > 0) {
			src_avail = MIXER_GetSequentialOutputBufferAvail();

			if (src_avail > 0) {
				input = &mixer.output_buffer[mixer.output_buffer_read][0];

				if (istep == (1 << MIXER_SHIFT)) {
					if (src_avail > need) src_avail = need;

					mixer.output_buffer_read += src_avail;
					assert(mixer.output_buffer_read <= MIXER_BUFSIZE);
					while (src_avail > 0) {
						*output++ = MIXER_CLIP(*input++ >> MIXER_SHIFT);
						*output++ = MIXER_CLIP(*input++ >> MIXER_SHIFT);
						src_avail--;
						need--;
					}
				}
				else {
					// TODO: Upgrade this code to linear interpolation!
					while (src_avail > 0 && need > 0) {
						*output++ = MIXER_CLIP(input[0] >> MIXER_SHIFT);
						*output++ = MIXER_CLIP(input[1] >> MIXER_SHIFT);
						ifrac += istep;
						need--;

						while (src_avail > 0 && ifrac > MIXER_REMAIN) {
							mixer.output_buffer_read++;
							ifrac -= MIXER_REMAIN+1;
							src_avail--;
							input += 2;
						}
					}
				}

				if (mixer.output_buffer_read >= MIXER_BUFSIZE)
					mixer.output_buffer_read = 0;
			}
			else {
				break;
			}
		}
	}

	if (need > 0) {
		memset(output,0,MIXER_SSIZE*need);
		need = 0;
	}
}

static void MIXER_Stop(Section* sec) {
}

class MIXER : public Program {
public:
	void MakeVolume(char * scan,float & vol0,float & vol1) {
		Bitu w=0;
		bool db=(toupper(*scan)=='D');
		if (db) scan++;
		while (*scan) {
			if (*scan==':') {
				++scan;w=1;
			}
			char * before=scan;
			float val=(float)strtod(scan,&scan);
			if (before==scan) {
				++scan;continue;
			}
			if (!db) val/=100;
			else val=powf(10.0f,(float)val/20.0f);
			if (val<0) val=1.0f;
			if (!w) {
				vol0=val;
			} else {
				vol1=val;
			}
		}
		if (!w) vol1=vol0;
	}

	void Run(void) {
		if(cmd->FindExist("/LISTMIDI")) {
			ListMidi();
			return;
		}
		if (cmd->FindString("MASTER",temp_line,false)) {
			MakeVolume((char *)temp_line.c_str(),mixer.mastervol[0],mixer.mastervol[1]);
		}
		MixerChannel * chan=mixer.channels;
		while (chan) {
			if (cmd->FindString(chan->name,temp_line,false)) {
				MakeVolume((char *)temp_line.c_str(),chan->volmain[0],chan->volmain[1]);
			}
			chan->UpdateVolume();
			chan=chan->next;
		}
		if (cmd->FindExist("/NOSHOW")) return;
		chan=mixer.channels;
		WriteOut("Channel  Main    Main(dB)\n");
		ShowVolume("MASTER",mixer.mastervol[0],mixer.mastervol[1]);
		for (chan=mixer.channels;chan;chan=chan->next) 
			ShowVolume(chan->name,chan->volmain[0],chan->volmain[1]);
	}
private:
	void ShowVolume(const char * name,float vol0,float vol1) {
		WriteOut("%-8s %3.0f:%-3.0f  %+3.2f:%-+3.2f \n",name,
			vol0*100,vol1*100,
			20*log(vol0)/log(10.0f),20*log(vol1)/log(10.0f)
		);
	}

	void ListMidi(){
#if defined (WIN32)
		unsigned int total = midiOutGetNumDevs();	
		for(unsigned int i=0;i<total;i++) {
			MIDIOUTCAPS mididev;
			midiOutGetDevCaps(i, &mididev, sizeof(MIDIOUTCAPS));
			WriteOut("%2d\t \"%s\"\n",i,mididev.szPname);
		}
#endif
	return;
	};

};

static void MIXER_ProgramStart(Program * * make) {
	*make=new MIXER;
}

MixerChannel* MixerObject::Install(MIXER_Handler handler,Bitu freq,const char * name){
	if(!installed) {
		if(strlen(name) > 31) E_Exit("Too long mixer channel name");
		safe_strncpy(m_name,name,32);
		installed = true;
		return MIXER_AddChannel(handler,freq,name);
	} else {
		E_Exit("already added mixer channel.");
		return 0; //Compiler happy
	}
}

MixerObject::~MixerObject(){
	if(!installed) return;
	MIXER_DelChannel(MIXER_FindChannel(m_name));
}

#ifdef WIN32
void MENU_swapstereo(bool enabled) {
	mixer.swapstereo=enabled;
}
#endif

void MIXER_Init() {
	AddExitFunction(AddExitFunctionFuncPair(MIXER_Stop));

	LOG(LOG_MISC,LOG_DEBUG)("Initializing DOSBox audio mixer");

	Section_prop * section=static_cast<Section_prop *>(control->GetSection("mixer"));
	/* Read out config section */
	mixer.freq=section->Get_int("rate");
	mixer.nosound=section->Get_bool("nosound");
	mixer.blocksize=section->Get_int("blocksize");
	mixer.swapstereo=section->Get_bool("swapstereo");
	mixer.sampleaccurate=section->Get_bool("sample accurate");

	/* Initialize the internal stuff */
	mixer.channels=0;
//	mixer.pos=0;
//	mixer.done=0;
//	memset(mixer.work,0,sizeof(mixer.work));
	mixer.mastervol[0]=1.0f;
	mixer.mastervol[1]=1.0f;

	/* Start the Mixer using SDL Sound at 22 khz */
	SDL_AudioSpec spec;
	SDL_AudioSpec obtained;

	spec.freq=mixer.freq;
	spec.format=AUDIO_S16SYS;
	spec.channels=2;
	spec.callback=MIXER_SDL_CallBack;
	spec.userdata=NULL;
	spec.samples=(Uint16)mixer.blocksize;

//	mixer.tick_remain=0;
	if (mixer.nosound) {
		LOG(LOG_MISC,LOG_DEBUG)("MIXER:No Sound Mode Selected.");
//		mixer.tick_add=((mixer.freq) << MIXER_SHIFT)/1000;
		TIMER_AddTickHandler(MIXER_Mix_NoSound);
	} else if (SDL_OpenAudio(&spec, &obtained) <0 ) {
		mixer.nosound = true;
		LOG(LOG_MISC,LOG_DEBUG)("MIXER:Can't open audio: %s , running in nosound mode.",SDL_GetError());
//		mixer.tick_add=((mixer.freq) << MIXER_SHIFT)/1000;
		TIMER_AddTickHandler(MIXER_Mix_NoSound);
	} else {
		if(((Bitu)mixer.freq != (Bitu)obtained.freq) || ((Bitu)mixer.blocksize != (Bitu)obtained.samples))
			LOG(LOG_MISC,LOG_DEBUG)("MIXER:Got different values from SDL: freq %d, blocksize %d",(int)obtained.freq,(int)obtained.samples);

		mixer.freq=obtained.freq;
		mixer.blocksize=obtained.samples;
		if (mixer.sampleaccurate) {
//			mixer.tick_add=1 << MIXER_SHIFT; /* one sample per tick */
			PIC_AddEvent(MIXER_MixPICEvent,1000.0 / mixer.freq,0);
		}
		else {
//			mixer.tick_add=(mixer.freq << MIXER_SHIFT)/1000;
			TIMER_AddTickHandler(MIXER_Mix);
		}
		SDL_PauseAudio(0);
	}
	mixer.min_needed=section->Get_int("prebuffer");
	if (mixer.min_needed>90) mixer.min_needed=90;
	mixer.min_needed=(mixer.freq*mixer.min_needed)/1000;
	mixer.max_needed=mixer.blocksize * 2 + 2*mixer.min_needed;
	mixer_start_pic_time = PIC_FullIndex();
	mixer_sample_counter = 0;

	// new mixer code
	mixer.freq_inv16b = (1 << 16) / mixer.freq;
	mixer.output_buffer_read = 0;
	mixer.output_buffer_write = 0;
	mixer.current_second_sample_counter = 0;
	mixer.samples_per_ms = mixer.freq / 1000;
	mixer.samples_per_ms_f = mixer.freq % 1000;
	mixer.current_sample_per_ms = mixer.samples_per_ms + (mixer.samples_per_ms_f != 0 ? 1 : 0);
	mixer.current_sample_f_count = 0;
	mixer.current_ms = 0;

	LOG(LOG_MISC,LOG_DEBUG)("Mixer: sample_accurate=%u blocksize=%u sdl_rate=%uHz mixer_rate=%uHz channels=%u samples=%u min/max=%u/%u samples_per_ms=%u.%03u",
		(unsigned int)mixer.sampleaccurate,
		(unsigned int)mixer.blocksize,
		(unsigned int)obtained.freq,
		(unsigned int)mixer.freq,
		(unsigned int)obtained.channels,
		(unsigned int)obtained.samples,
		(unsigned int)mixer.min_needed,
		(unsigned int)mixer.max_needed,
		(unsigned int)mixer.samples_per_ms,
		(unsigned int)mixer.samples_per_ms_f);

	PROGRAMS_MakeFile("MIXER.COM",MIXER_ProgramStart);
}

