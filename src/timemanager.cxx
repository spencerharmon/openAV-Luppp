/*
 * Author: Harry van Haaren 2013
 *         harryhaaren@gmail.com
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "timemanager.hxx"

#include <iostream>
#include <cstdio>

#include "buffers.hxx"
#include "eventhandler.hxx"

#include "observer/time.hxx"

#include "jack.hxx"

extern Jack* jack;

using namespace std;

TimeManager::TimeManager():
	transportState( TRANSPORT_ROLLING ),
	observers()
{
	samplerate = jack->getSamplerate();
	// 120 BPM default
	if(jack->_timebase_master){
		fpb = samplerate / 2;
		//Counter for current bar/beat
		barCounter  = 0;
		beatCounter = 0;

		previousBeat = 0;

		//In process() we want to immediately process bar(), beat() of all observers
		// thats why beatFrameCountdown<nframes, but we don't know yet what value nframes has
		// so set beatFrameCountdown to a value that garantees beatFrameCountdown<nframes
		beatFrameCountdown = -1;//fpb;

		totalFrameCounter = 0;
	}else{
//		client = jack->getJackClientPointer();
//		jack_position_t tpos;
//		jack_transport_state_t tstate = jack_transport_query( client, &tpos);
//		fpb = samplerate * 60 / tpos.beats_per_minute;
		fpb = 1;
	}

	tapTempoPos = 0;
	tapTempo[0] = 0;
	tapTempo[1] = 0;
	tapTempo[2] = 0;
}


int TimeManager::getFpb()
{
	return fpb;
}
int TimeManager::getBeatsPerBar()
{
	return beats_per_bar;
}

void TimeManager::setBpm(float bpm)
{
#ifdef DEBUG_TIME
	LUPPP_NOTE("%s %f","setBpm()",bpm);
#endif
	setFpb( samplerate / bpm * 60 );
	barCounter  = 0;
	beatCounter = 0;
	beatFrameCountdown = -1;
	/*
	for(int i=0;i<observers.size();i++)
	    observers[i]->resetTimeState();
	*/
}

void TimeManager::setBpmZeroOne(float b)
{
	setBpm( b * 160 + 60 ); // 60 - 220
}


void TimeManager::setFpb(float f)
{
	fpb = f;
	int bpm = ( samplerate * 60) / f;

	char buffer [50];
	sprintf (buffer, "TM, setFpb() %i, bpm = %i", int(f), int(bpm) );
	EventGuiPrint e( buffer );
	writeToGuiRingbuffer( &e );

	EventTimeBPM e2( bpm );
	writeToGuiRingbuffer( &e2 );

	for(uint i = 0; i < observers.size(); i++) {
		observers.at(i)->setFpb(fpb);
	}
}

void TimeManager::registerObserver(TimeObserver* o)
{
	//LUPPP_NOTE("%s","registerObserver()");
	observers.push_back(o);
	o->setFpb( fpb );

	int bpm = ( samplerate * 60) / fpb;
	EventTimeBPM e2( bpm );
	writeToGuiRingbuffer( &e2 );
}

void TimeManager::tap()
{
	// reset tap tempo to "first tap" if more than 5 secs elapsed since last tap
	if ( tapTempo[0] < totalFrameCounter - samplerate * 5 ) {
		tapTempoPos = 0;
	}

	if ( tapTempoPos < 3 ) {
		tapTempo[tapTempoPos] = totalFrameCounter;
		tapTempoPos++;
	} else {
		// calculate frames per tap
		int tapFpb1 = tapTempo[1] - tapTempo[0];
		int tapFpb2 = tapTempo[2] - tapTempo[1];
		int tapFpb3 = totalFrameCounter - tapTempo[2]; // last tap, until now

		int average = (tapFpb1 + tapFpb2 + tapFpb3) / 3;

		if( average < 13000 ) {
			char buffer [50];
			sprintf (buffer, "TM, tap() average too slow! quitting");
			EventGuiPrint e( buffer );
			writeToGuiRingbuffer( &e );
			return;
		}

		char buffer [50];
		sprintf (buffer, "TM, tap() average = %i", average );
		EventGuiPrint e( buffer );
		writeToGuiRingbuffer( &e );


		setFpb(average);

		// reset, so next 3 taps restart process
		tapTempoPos = 0;
	}
}

int TimeManager::getNframesToBeat()
{
	// FIXME
	return -1; //beatFrameCountdown;
}

void TimeManager::setTransportState( TRANSPORT_STATE s )
{
	transportState = s;
	if(transportState == TRANSPORT_STOPPED)
		jack->transportRolling(false);
	else {
		jack->transportRolling(true);
		if(jack->_timebase_master){
			barCounter  = 0;
			beatCounter = 0;
			beatFrameCountdown = -1;
			for(unsigned int i=0; i<observers.size(); i++)
				observers[i]->resetTimeState();
		}
	}
}

void TimeManager::masterProcess(Buffers* buffers)
{
	// time signature?
	//buffers->transportPosition->beats_per_bar = 4;
	//buffers->transportPosition->beat_type     = 4;
	if ( transportState == TRANSPORT_STOPPED ) {
		return;
	}
	int nframes = buffers->nframes;
	if ( beatFrameCountdown < nframes ) {
		//length of beat is not multiple of nframes, so need to process last frames of last beat *before* setting next beat
		//then set new beat (get the queued actions: play, rec etc)
		// then process first frames *after* new beat
		int before=(beatCounter*fpb)%nframes;
		int after=nframes-before;

		if ( before < nframes && after <= nframes && before + after == nframes ) {
			char buffer [50];
//      sprintf (buffer, "Timing OK: before %i, after %i, b+a %i",  before, after, before+after );
//      EventGuiPrint e2( buffer );
//      writeToGuiRingbuffer( &e2 );

		} else {
			char buffer [50];
			sprintf (buffer, "Timing Error: before: %i, after %i", before, after );
			EventGuiPrint e2( buffer );
			writeToGuiRingbuffer( &e2 );
		}

		// process before beat:
		if(before)
			jack->processFrames( before );

		// handle beat:
		// inform observers of new beat FIRST
		for(uint i = 0; i < observers.size(); i++) {
			observers.at(i)->beat();
		}

		if(buffers->transportPosition){
			if ( beatCounter % (int)buffers->transportPosition->beats_per_bar == 0 ) {
				// inform observers of new bar SECOND
				for(uint i = 0; i < observers.size(); i++) {
					observers.at(i)->bar();
				}
				barCounter++;
				//beatCounter=0;
			}
		}

		// process after
		// we need to clear internal buffers in order to write *after* frames to them
		jack->clearInternalBuffers(nframes);
		if(after)
			jack->processFrames( after );

		// write new beat to UI (bar info currently not used)
		EventTimeBarBeat e( barCounter, beatCounter );
		writeToGuiRingbuffer( &e );


		beatFrameCountdown = fpb-after;
		beatCounter++;
	} else {
		jack->processFrames( nframes );
		beatFrameCountdown -= nframes;
	}
	totalFrameCounter += nframes;
	// write BPM / transport info to JACK

	int bpm = ( samplerate * 60) / fpb;
	if ( buffers->transportPosition ) {
			buffers->transportPosition->valid = (jack_position_bits_t)(JackPositionBBT | JackTransportPosition);

			buffers->transportPosition->bar  = beatCounter / 4 + 1;// bars 1-based
			buffers->transportPosition->beat = (beatCounter % 4) + 1; // beats 1-4

			float part = float( fpb-beatFrameCountdown) / fpb;
			buffers->transportPosition->tick = part > 1.0f? 0.9999*1920 : part*1920;

			buffers->transportPosition->frame = totalFrameCounter;

			buffers->transportPosition->ticks_per_beat = 1920;
			buffers->transportPosition->beats_per_bar = 4;

			buffers->transportPosition->beats_per_minute = bpm;
	}
}
void TimeManager::clientProcess(Buffers* buffers)
{
	jack_client_t * client = jack->getJackClientPointer();
	int nframes = buffers->nframes;

	jack_transport_state_t tstate;
	jack_position_t tpos;
	tstate = jack_transport_query( client, &tpos);

	beats_per_bar = tpos.beats_per_bar;
	bool bpmchange = 0;
	int thisbeat = ((int)tpos.bar - 1) * (int)tpos.beats_per_bar + ((int)tpos.beat - 1);

	//detect and react to bpm change by master.
	//should not be done with already loaded looperclips; may lead to unexpected behavior
	//no time stretching or compensation will occur, so loaded audio will be out of sync with master.
	if( tpos.beats_per_minute != bpmLastCycle){
		//setBpm shouldn't be used by jack timebase client because it resets counters. use setFpb directly.
//		setFpb( samplerate / tpos.beats_per_minute * 60 ); This is wrong, right?
		setFpb( samplerate * 60 / tpos.beats_per_minute );
		bpmLastCycle = tpos.beats_per_minute;

		nextBeatFrame = (thisbeat + 1) * fpb;

		//preapare beat length detection for bpm change
		int iter = (totalbeatcounter - 1) % 100;
		if(iter == 0)
			iter = 100;
		for( int i = 0; i <= iter; i++){
			obsFpb[i] = 0;
		}
		//buffer start of detection due to anomalous beat length after change
		totalbeatcounter = -2;
		bpmchange = true;
	}

	if(tstate == JackTransportStopped){
		setTransportState(TRANSPORT_STOPPED);
		lastbeat = 0;
	}else if(tstate == JackTransportRolling){
		setTransportState(TRANSPORT_ROLLING);
	}
	if ( transportState == TRANSPORT_STOPPED ) {
		return;
	}

	//adjust to zero-based for frame maths

	jack_nframes_t startframe = tpos.frame;
	jack_nframes_t endframe = startframe + nframes - 1;

	//following block is intended to detect the observed beat length in samples and
	//adjust the fpb to reflect the average observed beat length rather than the reported one.
	//I'm not sure if the difference between the expected fpb and the observed one is
	//due to a misunderstanding on my part or bad behavior from some timebase masters.
	//this is effectively implemented as a finite state machine, with the logic playing out
	//over many cycles.
	//the margin of error for beat length detection is equal to nframes.

	int nextbeatskew = 0;
	int beat = tpos.beat;
	bool thisisarealbeat = false;
	bool requestbeat = false;
	if(beat != lastbeat && lastbeat > 0){
		//record and average observed beat lengths
		if(totalbeatcounter > 0){
			//iteration
			int iter = (totalbeatcounter - 1) % 100;
			//store observed beat length
			obsFpb[ iter ] = startframe - estLastBeatStor;

			//denominator; start over at 100 (101 lengths: do more or less?)
			int denom;
			if( totalbeatcounter >= 100){
				denom = 100;
			}else denom = iter;

			//agerage stored lengths and set fpb
			int totalObsFpb = 0;
			for( int i = 0; i <= denom; i++ ){
				totalObsFpb += obsFpb[i];
			}
			setFpb( totalObsFpb / (denom + 1) );

			//char buffer[50];
			//sprintf (buffer, "observed beat length: %i", startframe - estLastBeatStor);
			//EventGuiPrint e1( buffer );
			//writeToGuiRingbuffer( &e1 );

		}
		//changing to median to reduce error to nframes/2
		estLastBeatStor = startframe + (nframes / 2);
		++totalbeatcounter;

		if( nextBeatFrame > endframe){
			//avg too long, beat already occured
			endframe = nextBeatFrame;
			startframe = endframe - nframes + 1;
			requestbeat = true;
			nextbeatskew -= nextBeatFrame - endframe;
		}
		if( nextBeatFrame < startframe){
			//not the "real" start frame, but this allows us to process the beat
			//while adjusting the skew to aid beat length detection
			startframe = nextBeatFrame;
			endframe = startframe + nframes -1;

			//don't do above.
			//when the average of the beat lengths is too low, this can occur
			//several cycles in a row until the fpb is greater than or equal
			//to the ideal, erroneously requesting beats.
//			if( startframe > lastbeatframe + fpb * 0.9 ){
				//register beat.
			requestbeat = true;
			nextbeatskew +=  startframe - nextBeatFrame;
//			}
		}
		if( startframe <= nextBeatFrame && endframe >= nextBeatFrame ) {
			//normally detected beat
			requestbeat = true;
		}
	}

	//ensure that beats aren't registered at times they shouldn't
	if(requestbeat){
		int lastbeatframe = nextBeatFrame - fpb*2;

		char buffer[50];
		sprintf(buffer, "beat requested; lastbeatframe: %i", lastbeatframe);
		EventGuiPrint e0( buffer );
		writeToGuiRingbuffer( &e0 );

		sprintf(buffer, "startframe: %i", startframe);
		EventGuiPrint e1( buffer );
		writeToGuiRingbuffer( &e1 );

		sprintf(buffer, "threshold: %f", lastbeatframe + fpb * 0.9);
		EventGuiPrint e2( buffer );
		writeToGuiRingbuffer( &e2 );

		if( bpmchange ){
			//allow beat change on bpmchange
			thisisarealbeat = true;
		} else if( startframe > lastbeatframe + fpb * 0.9){
			//most of a beat has passed
			thisisarealbeat = true;
		}
	}

	if ( thisisarealbeat ) {
		//length of beat is not multiple of nframes, so need to process last frames of last beat *before* setting next beat
		//then set new beat (get the queued actions: play, rec etc)
		// then process first frames *after* new beat

		int before = nextBeatFrame - startframe;
		int after = endframe - nextBeatFrame + 1;

		if ( before < nframes && after <= nframes && before + after == nframes ) {
			char buffer [50];
		} else {
			char buffer [50];
			sprintf (buffer, "Timing Error: before: %i, after %i", before, after );
			EventGuiPrint e2( buffer );
			writeToGuiRingbuffer( &e2 );
		}

		// process before beat:
		if(before)
			jack->processFrames( before );

		// handle beat:
		// inform observers of new beat FIRST
		for(uint i = 0; i < observers.size(); i++) {
			observers.at(i)->beat();
		}

		//inform of new bar
		if ( lastbeat == tpos.beats_per_bar ) {
			// inform observers of new bar SECOND
			for(uint i = 0; i < observers.size(); i++) {
				observers.at(i)->bar();
			}
		}

		// process after
		// we need to clear internal buffers in order to write *after* frames to them
		jack->clearInternalBuffers(nframes);
		if(after)
			jack->processFrames( after );

		//nextbeatframe should only change each beat
		nextBeatFrame += fpb + nextbeatskew;

		// write new beat to UI (bar info currently not used)
		EventTimeBarBeat e( tpos.bar, tpos.beat );
		writeToGuiRingbuffer( &e );
	} else {
		jack->processFrames( nframes );
	}
	lastbeat = beat;
}
void TimeManager::process(Buffers* buffers)
{
	if(jack->_timebase_master){
		masterProcess(buffers);
	}else{
		clientProcess(buffers);
	}
}

