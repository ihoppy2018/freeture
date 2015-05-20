/*
								AcqThread.cpp

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*
*	This file is part of:	freeture
*
*	Copyright:		(C) 2014-2015 Yoan Audureau -- FRIPON-GEOPS-UPSUD
*
*	License:		GNU General Public License
*
*	FreeTure is free software: you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation, either version 3 of the License, or
*	(at your option) any later version.
*	FreeTure is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*	You should have received a copy of the GNU General Public License
*	along with FreeTure. If not, see <http://www.gnu.org/licenses/>.
*
*	Last modified:		21/01/2015
*
*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

/**
* \file    AcqThread.cpp
* \author  Yoan Audureau -- FRIPON-GEOPS-UPSUD
* \version 1.0
* \date    21/01/2015
* \brief   Acquisition thread.
*/

#include "AcqThread.h"

boost::log::sources::severity_logger< LogSeverityLevel >  AcqThread::logger;
AcqThread::_Init AcqThread::_initializer;

AcqThread::AcqThread(	CamType									camType,
						boost::mutex							*cfg_m,
						string									cfg_p,
					    boost::circular_buffer<Frame>           *fb,
						boost::mutex                            *fb_m,
						boost::condition_variable               *fb_c,
						bool									*sSignal,
						boost::mutex                            *sSignal_m,
						boost::condition_variable               *sSignal_c,
						bool                                    *dSignal,
						boost::mutex                            *dSignal_m,
						boost::condition_variable               *dSignal_c,
						DetThread	                            *detection,
                        StackThread	                            *stack){

    acquisitionThread			    = NULL;
    mustStop				        = false;
	cam								= NULL;
    cfg_mutex						= cfg_m;
	cfg_path						= cfg_p;
	srcType							= camType;
    frameBuffer                     = fb;
    frameBuffer_mutex               = fb_m;
    frameBuffer_condition           = fb_c;

    stackSignal						= sSignal;
    stackSignal_mutex				= sSignal_m;
    stackSignal_condition			= sSignal_c;

    detSignal						= dSignal;
    detSignal_mutex                 = dSignal_m;
    detSignal_condition             = dSignal_c;

    frameCpt                        = 0;
    nbFailGrabbedFrames             = 0;
    nbSuccessGrabbedFrames          = 0;

	threadTerminated				= false;

	detectionProcess                = detection;
	stackProcess                    = stack;

	autoExposure = NULL;

	enableStackThread = false;

}

AcqThread::~AcqThread(void){

    if(cam != NULL) delete cam;

	if(acquisitionThread != NULL) delete acquisitionThread;

	if(autoExposure != NULL) delete autoExposure;

}

void AcqThread::join(){

	acquisitionThread->join();

}

void AcqThread::stopThread(){

	// Signal the thread to stop (thread-safe)
	mustStopMutex.lock();
	mustStop = true;
	mustStopMutex.unlock();

	// Wait for the thread to finish.
    if(acquisitionThread != NULL)
	while(acquisitionThread->timed_join(boost::posix_time::seconds(2)) == false){

        acquisitionThread->interrupt();

    }

}

bool AcqThread::startThread(){

	BOOST_LOG_SEV(logger,normal) << "Create new Device.";

	cam = new Device(srcType);

	BOOST_LOG_SEV(logger,normal) << "Prepare device.";
	boost::mutex::scoped_lock lock_(*cfg_mutex);
	if(!cam->prepareDevice(srcType, cfg_path)){
		lock_.unlock();
		BOOST_LOG_SEV(logger,fail) << "Fail to prepare device.";
		return false;
	}
	lock_.unlock();
	BOOST_LOG_SEV(logger,normal) << "Sucess to prepare device.";
	BOOST_LOG_SEV(logger,normal) << "Create acquisition thread.";

    acquisitionThread = new boost::thread(boost::ref(*this));
	return true;

}

bool AcqThread::getThreadTerminatedStatus(){

	return threadTerminated;

}

void AcqThread::operator()(){

	bool stop = false;
	vector<string> frameDate;   // Date in string vector : YYYY  MM  DD  hh  mm  ss
    string accurateFrameDate;   // Date in string : YYYY-MM-DDTHH:MM:SS,fffffffff

	BOOST_LOG_SCOPED_THREAD_TAG("LogName", "ACQ_THREAD");
	BOOST_LOG_SEV(logger,notification) << "\n";
	BOOST_LOG_SEV(logger,notification) << "==============================================";
	BOOST_LOG_SEV(logger,notification) << "========== Start acquisition thread ==========";
	BOOST_LOG_SEV(logger,notification) << "==============================================";

    /// Prepare scheduled long acquisition.

    ACQ_SCHEDULE = cam->getSchedule();      // Get acquisition schedule.
    sortAcquisitionSchedule();              // Order schedule times.
    selectNextAcquisitionSchedule();        // Search next acquisition according to the current time.
    bool scheduleTaskStatus = false;
    bool scheduleTaskActive = false;

    /// Prepare acquisition at regular time interval.

    int regularAcqFrameInterval = 0;
    int regularAcqFrameCounter = 0;

    if(cam->getAcqRegularEnabled())

        regularAcqFrameInterval = cam->getAcqRegularTimeInterval() * cam->getFPS();

    /// Exposure adjustment variables.

    bool exposureControlStatus = false;
    bool exposureControlActive = false;
    bool cleanStatus = false;

    if(cam->getAcqDayEnabled())

        autoExposure = new ExposureControl(cam->getExposureControlFrequency(),
                                           cam->getExposureControlSaveImage(),
                                           cam->getExposureControlSaveInfos(),
                                           cam->getDataPath(),
                                           cam->getStationName());

    int timeStartSunrise    = 0;
    int timeStopSunrise     = 0;
    int timeStartSunset     = 0;
    int timeStopSunset      = 0;

    if(cam->getSunriseTime().size() != 0 && cam->getSunsetTime().size() != 0){

        timeStartSunrise    = cam->getSunriseTime().at(0) * 3600 + cam->getSunriseTime().at(1) * 60;
        timeStopSunrise     = timeStartSunrise + cam->getSunriseDuration() * 2;
        timeStartSunset     = cam->getSunsetTime().at(0) * 3600 + cam->getSunsetTime().at(1) * 60;
        timeStopSunset      = timeStartSunset + cam->getSunsetDuration() * 2;

    }

    /// Enable or disable stack thread in daytime.

    if(stackProcess != NULL)
        enableStackThread = true;
    else
        enableStackThread = false;

    cout << "enableStackThread = " << enableStackThread << endl;

    /// Acquisition process.

	try{

	    do{

            // Load videos file or frames directory if input type is : FRAMES or VIDEO
	        if(!cam->loadDataset()) break;

            do{

                if(cam->getDisplayInput())
                    namedWindow("Display window", WINDOW_NORMAL );

                Frame newFrame;
                bool grabStatus = false;

                double tacq = (double)getTickCount();

                if(cam->grabImage(newFrame)){

                    grabStatus = true;

                    BOOST_LOG_SEV(logger, normal) << "============= FRAME " << newFrame.getNumFrame() << " ============= ";
                    cout << "============= FRAME " << newFrame.getNumFrame() << " ============= " << endl;

                    // Get date.
                    frameDate = newFrame.getDateString();
                    accurateFrameDate = newFrame.getAcqDateMicro();

                    // Check if exposure control is active.
                    if(!exposureControlStatus /*|| cam->getVideoFramesInput()*/){

                        // Exposure control is not active, the new frame can be shared with others threads.

                        boost::mutex::scoped_lock lock(*frameBuffer_mutex);
                        frameBuffer->push_back(newFrame);
                        lock.unlock();

                        boost::mutex::scoped_lock lock2(*detSignal_mutex);
                        *detSignal = true;
                        detSignal_condition->notify_one();
                        lock2.unlock();

                        if(enableStackThread){

                            boost::mutex::scoped_lock lock3(*stackSignal_mutex);
                            *stackSignal = true;
                            stackSignal_condition->notify_one();
                            lock3.unlock();

                        }

                        cleanStatus = false;

                    }else{

                        // Exposure control is active, the new frame can't be shared with others threads.
                        if(!cleanStatus){

                            // If stack process exists.
                            if(stackProcess != NULL){

                                boost::mutex::scoped_lock lock(*stackSignal_mutex);
                                *stackSignal = false;
                                lock.unlock();

                                // Force interruption.
                                cout << "Send interruption signal to stack " << endl;
                                stackProcess->interruptThread();

                            }

                            // If detection process exists
                            if(detectionProcess != NULL){

                                boost::mutex::scoped_lock lock(*detSignal_mutex);
                                *detSignal = false;
                                lock.unlock();
                                cout << "Sending interruption signal to detection process... " << endl;
                                detectionProcess->interruptThread();

                            }

                            // Reset framebuffer.
                            cout << "Cleaning frameBuffer..." << endl;
                            boost::mutex::scoped_lock lock(*frameBuffer_mutex);
                            frameBuffer->clear();
                            lock.unlock();

                            cleanStatus = true;

                        }

                    }

                    if(autoExposure != NULL && exposureControlActive)
                        exposureControlStatus = autoExposure->controlExposureTime(cam, newFrame.getImg(), accurateFrameDate);

                    if(cam->getDisplayInput())
                        imshow("Display window", newFrame.getImg());

                }else{

                    BOOST_LOG_SEV(logger, notification) << "> Fail to grab frame";
                    nbFailGrabbedFrames++;

                }

                if(grabStatus && !cam->getVideoFramesInput()){

                    int currentTimeInSec = atoi(frameDate.at(3).c_str()) * 3600 + atoi(frameDate.at(4).c_str()) * 60 + atoi(frameDate.at(5).c_str());
                    /*cout << " TimeInSec : " << currentTimeInSec << endl;
                    cout << " StartSunrise : " << timeStartSunrise << endl;
                    cout << " StopSunrise  : " << timeStopSunrise << endl;
                    cout << " StartSunset  : " << timeStartSunset << endl;
                    cout << " StopSunset   : " << timeStopSunset << endl;*/

                    // If acquisition at regular time interval is enabled.
                    if(cam->getAcqRegularEnabled()){

                        if((currentTimeInSec > timeStopSunset) || (currentTimeInSec < timeStartSunrise)){

                            if(regularAcqFrameCounter >= regularAcqFrameInterval){

                                BOOST_LOG_SEV(logger, notification) << "Run regular acquisition.";
                                runRegularAcquisition(accurateFrameDate);
                                sleep(1);
                                regularAcqFrameCounter = 0;

                            }else{

                                cout << "Next regular acquisition in : " << regularAcqFrameInterval - regularAcqFrameCounter << " frames." << endl;
                                regularAcqFrameCounter++;

                            }

                        }else{

                            regularAcqFrameCounter = 0;

                        }

                    }

                    // Check schedule for long exposure time captures.
                    if(ACQ_SCHEDULE.size() != 0 && cam->getAcqScheduleEnabled()){

                        // Time for a long exposure time acquisition.
                        if(nextTask.getH() == atoi(frameDate.at(3).c_str()) && nextTask.getM() == atoi(frameDate.at(4).c_str()) && atoi(frameDate.at(5).c_str()) == nextTask.getS()){

                            nextTask.setDate(accurateFrameDate);

                            // Launch single acquisition
                            bool result = runScheduledAcquisition(nextTask);

                            sleep(1);

                            // Update nextTask
                            selectNextAcquisitionSchedule();

                        }else{

                            // The current time elapsed.
                            if(atoi(frameDate.at(3).c_str()) > nextTask.getH()){

                               selectNextAcquisitionSchedule();

                            }else if(atoi(frameDate.at(3).c_str()) == nextTask.getH()){

                                if(atoi(frameDate.at(4).c_str()) > nextTask.getM()){

                                    selectNextAcquisitionSchedule();

                                }else if(atoi(frameDate.at(4).c_str()) == nextTask.getM()){

                                    if(atoi(frameDate.at(5).c_str()) > nextTask.getS()){

                                        selectNextAcquisitionSchedule();

                                    }

                                }

                            }

                        }

                    }

                    // If day acquisition enabled, enable or disable exposure control if it's sunset or sunrise.
                    if(cam->getAcqDayEnabled()){

                        cout << "exposureControlActive : " << exposureControlActive << endl;

                        // Check sunrise and sunset time.
                        if((currentTimeInSec > timeStartSunrise && currentTimeInSec < timeStopSunrise) || (currentTimeInSec > timeStartSunset && currentTimeInSec < timeStopSunset)){

                            exposureControlActive = true;
                            cout << "SUNSET or SUNRISE ! "<< endl;

                        }else{

                            if(exposureControlActive){

                                if((currentTimeInSec >= timeStopSunrise && currentTimeInSec < timeStartSunset)){

                                    cout << "DAYTIME ! "<< endl;
                                    BOOST_LOG_SEV(logger, notification) << "Apply day exposure time.";
                                    cout << cam->getDayExposureTime()<< endl;
                                    cam->setExposureTime(cam->getDayExposureTime());
                                    cout << cam->getDayGain()<< endl;
                                    cam->setGain(cam->getDayGain());


                                }else if((currentTimeInSec >= timeStopSunset) || (currentTimeInSec < timeStartSunrise)){

                                    cout << "NIGHT ! "<< endl;
                                    BOOST_LOG_SEV(logger, notification) << "Apply night exposure time.";
                                    cout << cam->getNightExposureTime()<< endl;
                                    cam->setExposureTime(cam->getNightExposureTime());
                                    cout << cam->getNightGain()<< endl;
                                    cam->setGain(cam->getNightGain());
                                }

                            }

                            exposureControlActive = false;
                            exposureControlStatus = false;

                        }
                    }

                    // Asleep or wake up stack process
                    if(stackProcess != NULL){

                        if((currentTimeInSec > timeStopSunset) || (currentTimeInSec < timeStartSunrise)){

                            enableStackThread = true;
                            cout << "enableStackThread night = " << enableStackThread << endl;

                        }else{

                            if(enableStackThread){

                                boost::mutex::scoped_lock lock(*stackSignal_mutex);
                                *stackSignal = false;
                                lock.unlock();

                                // Force interruption.
                                cout << "Send interruption signal to stack " << endl;
                                stackProcess->interruptThread();

                            }

                            enableStackThread = false;

                            cout << "enableStackThread day = " << enableStackThread << endl;

                        }

                    }

                }

                tacq = (((double)getTickCount() - tacq)/getTickFrequency())*1000;
                std::cout << " [ TIME ACQ ] : " << tacq << " ms" << endl;
                BOOST_LOG_SEV(logger, normal) << " [ TIME ACQ ] : " << tacq << " ms";

                mustStopMutex.lock();
                stop = mustStop;
                mustStopMutex.unlock();

            }while(stop == false && !cam->getDeviceStopStatus());

            waitKey(1000);

            //cout << "Clear detection method." << endl;
            if(detectionProcess != NULL){

                detectionProcess->getDetMethod()->resetDetection();
                detectionProcess->getDetMethod()->resetMask();

                if(!detectionProcess->getRunStatus())
                    break;

            }

            cout << "Clear framebuffer" << endl;
            boost::mutex::scoped_lock lock(*frameBuffer_mutex);
            frameBuffer->clear();
            lock.unlock();

            cout << "Waiting for 5 seconds ..." << endl;
            waitKey(2000);

	    }while(cam->getDatasetStatus());

	}catch(const boost::thread_interrupted&){

			BOOST_LOG_SEV(logger,notification) << "Acquisition Thread INTERRUPTED";
            cout << "Acquisition Thread INTERRUPTED" <<endl;

    }catch(exception& e){

        cout << "An exception occured : " << e.what() << endl;
        BOOST_LOG_SEV(logger, critical) << e.what();

    }

    cam->acqStop();
	cam->grabStop();

	threadTerminated = true;

	std::cout << "Acquisition Thread terminated." << endl;
	BOOST_LOG_SEV(logger,notification) << "Acquisition Thread TERMINATED";

}

void AcqThread::selectNextAcquisitionSchedule(){

    if(ACQ_SCHEDULE.size() != 0){

        /// Search next acquisition according to the current date
        // Get current date.
        string currentDate = TimeDate::localDateTime(microsec_clock::universal_time(),"%Y:%m:%d:%H:%M:%S");
        cout << endl << "current date : " << currentDate << endl;
        vector<string> currentDateSplit;

        // Split date
        typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
        boost::char_separator<char> sep(":");
        tokenizer tokens(currentDate, sep);
        for (tokenizer::iterator tok_iter = tokens.begin();tok_iter != tokens.end(); ++tok_iter){
            currentDateSplit.push_back(*tok_iter);
        }

        // Get current Hour.
        int currentH = atoi(currentDateSplit.at(3).c_str());

        // Get current Minutes.
        int currentM = atoi(currentDateSplit.at(4).c_str());

        // Get current Seconds.
        int currentS = atoi(currentDateSplit.at(5).c_str());

        // Search next acquisition
        for(int i = 0; i < ACQ_SCHEDULE.size(); i++){

            if(currentH < ACQ_SCHEDULE.at(i).getH()){

               indexNextTask = i;
               break;

            }else if(currentH == ACQ_SCHEDULE.at(i).getH()){

                if(currentM < ACQ_SCHEDULE.at(i).getM()){

                    indexNextTask = i;
                    break;

                }else if(currentM == ACQ_SCHEDULE.at(i).getM()){

                    if(currentS < ACQ_SCHEDULE.at(i).getS()){

                        indexNextTask = i;
                        break;

                    }

                }

            }

        }

        nextTask = ACQ_SCHEDULE.at(indexNextTask);

        cout << "nextTask : " << nextTask.getH() << "H " << nextTask.getM() << "M" << nextTask.getS() << "S" << endl;

    }

}

void AcqThread::sortAcquisitionSchedule(){

    if(ACQ_SCHEDULE.size() != 0){

        // Sort time in list.
        vector<AcqSchedule> tempSchedule;

        do{

            int minH; int minM; int minS; bool init = false;

            vector<AcqSchedule>::iterator it;
            vector<AcqSchedule>::iterator it_select;

            for(it = ACQ_SCHEDULE.begin(); it != ACQ_SCHEDULE.end(); ++it){

                if(!init){

                    minH = (*it).getH();
                    minM = (*it).getM();
                    minS = (*it).getS();
                    it_select = it;
                    init = true;

                }else{

                    if((*it).getH() < minH){

                        minH = (*it).getH();
                        minM = (*it).getM();
                        minS = (*it).getS();
                        it_select = it;

                    }else if((*it).getH() == minH){

                        if((*it).getM() < minM){

                            minH = (*it).getH();
                            minM = (*it).getM();
                            minS = (*it).getS();
                            it_select = it;

                        }else if((*it).getM() == minM){

                            if((*it).getS() < minS){

                                minH = (*it).getH();
                                minM = (*it).getM();
                                minS = (*it).getS();
                                it_select = it;

                            }

                        }

                    }

                }

            }

            if(init){

                tempSchedule.push_back((*it_select));
                cout << "-> " << (*it_select).getH() << "H " << (*it_select).getM() << "M " << (*it_select).getS() << "S " << endl;
                ACQ_SCHEDULE.erase(it_select);

            }

        }while(ACQ_SCHEDULE.size() != 0);

        ACQ_SCHEDULE = tempSchedule;

    }

}

bool AcqThread::buildRegularAcquisitionDirectory(string YYYYMMDD){

	namespace fs = boost::filesystem;
	string	root		= cam->getDataPath() + cam->getStationName() + "_" + YYYYMMDD +"/";

	cout << "root : " << root<< endl;

    string	subDir		= "captures/";
    string	finalPath	= root + subDir;

	completeDataPath	= finalPath;
	BOOST_LOG_SEV(logger,notification) << "CompleteDataPath : " << completeDataPath;

    path p(cam->getDataPath());
    path p1(root);
    path p2(root + subDir);

    // If DATA_PATH exists
    if(fs::exists(p)){

        // If DATA_PATH/STATION_YYYYMMDD/ exists
        if(fs::exists(p1)){

            // If DATA_PATH/STATION_YYYYMMDD/astro/ doesn't exists
            if(!fs::exists(p2)){

                // If fail to create DATA_PATH/STATION_YYYYMMDD/astro/
                if(!fs::create_directory(p2)){

                    BOOST_LOG_SEV(logger,critical) << "Unable to create astro directory : " << p2.string();
                    return false;

                // If success to create DATA_PATH/STATION_YYYYMMDD/astro/
                }else{

                   BOOST_LOG_SEV(logger,notification) << "Success to create astro directory : " << p2.string();
                   return true;

                }
            }

        // If DATA_PATH/STATION_YYYYMMDD/ doesn't exists
        }else{

            // If fail to create DATA_PATH/STATION_YYYYMMDD/
            if(!fs::create_directory(p1)){

                BOOST_LOG_SEV(logger,fail) << "Unable to create STATION_YYYYMMDD directory : " << p1.string();
				return false;

            // If success to create DATA_PATH/STATION_YYYYMMDD/
            }else{

                BOOST_LOG_SEV(logger,notification) << "Success to create STATION_YYYYMMDD directory : " << p1.string();

                // If fail to create DATA_PATH/STATION_YYYYMMDD/astro/
                if(!fs::create_directory(p2)){

                    BOOST_LOG_SEV(logger,critical) << "Unable to create astro directory : " << p2.string();
					return false;

                // If success to create DATA_PATH/STATION_YYYYMMDD/astro/
                }else{

                    BOOST_LOG_SEV(logger,notification) << "Success to create astro directory : " << p2.string();
					return true;

                }
            }
        }

    // If DATA_PATH doesn't exists
    }else{

        // If fail to create DATA_PATH
        if(!fs::create_directory(p)){

            BOOST_LOG_SEV(logger,fail) << "Unable to create DATA_PATH directory : " << p.string();
			return false;

        // If success to create DATA_PATH
        }else{

            BOOST_LOG_SEV(logger,notification) << "Success to create DATA_PATH directory : " << p.string();

            // If fail to create DATA_PATH/STATION_YYYYMMDD/
            if(!fs::create_directory(p1)){

                BOOST_LOG_SEV(logger,fail) << "Unable to create STATION_YYYYMMDD directory : " << p1.string();
				return false;

            // If success to create DATA_PATH/STATION_YYYYMMDD/
            }else{

                BOOST_LOG_SEV(logger,notification) << "Success to create STATION_YYYYMMDD directory : " << p1.string();

                // If fail to create DATA_PATH/STATION_YYYYMMDD/astro/
                if(!fs::create_directory(p2)){

                    BOOST_LOG_SEV(logger,critical) << "Unable to create astro directory : " << p2.string();
					return false;

                // If success to create DATA_PATH/STATION_YYYYMMDD/astro/
                }else{

                    BOOST_LOG_SEV(logger,notification) << "Success to create astro directory : " << p2.string();
					return true;

                }
            }
        }
    }
}

bool AcqThread::runScheduledAcquisition(AcqSchedule task){

    // Stop camera
    cout << "Stopping camera..." << endl;
    cam->acqStop();
	cam->grabStop();

    // If stack process exists.
    if(stackProcess != NULL && enableStackThread){

        boost::mutex::scoped_lock lock(*stackSignal_mutex);
        *stackSignal = false;
        lock.unlock();

        // Force interruption.
        cout << "Send interruption signal to stack " << endl;
        stackProcess->interruptThread();


    }

    // If detection process exists
    if(detectionProcess != NULL){

        boost::mutex::scoped_lock lock(*detSignal_mutex);
        *detSignal = false;
        lock.unlock();
        cout << "Sending interruption signal to detection process " << endl;
        detectionProcess->interruptThread();

    }

    // Reset framebuffer.
    cout << "Cleaning frameBuffer..." << endl;
    boost::mutex::scoped_lock lock(*frameBuffer_mutex);
    frameBuffer->clear();
    lock.unlock();

    for(int i = 0; i < task.getN(); i++){

        // Configuration pour single capture
        Frame frame;
        cout << "Exposure : " << task.getE() << endl;
        frame.setExposure(task.getE());
        cout << "Gain : " << task.getG() << endl;
        frame.setGain(task.getG());
        CamBitDepth camFormat;
        cout << "Format : " << task.getF() << endl;
        Conversion::intBitDepth_To_CamBitDepth(task.getF(), camFormat);
        frame.setBitDepth(camFormat);

        // Single capture.
        if(cam->grabSingleImage(frame, cam->getCameraId())){

            cout << endl << "Single capture succeed !" << endl;

            /// ---------------------- Save grabbed frame --------------------------

            // Save the frame in Fits 2D.
            if(frame.getImg().data){

                string	YYYYMMDD = TimeDate::get_YYYYMMDD_fromDateString(task.getDate());
                cout << "YYYYMMDD : " << YYYYMMDD << endl;
                if(buildRegularAcquisitionDirectory(YYYYMMDD)){

                    cout << "Saving fits file ..." << endl;

                    Fits2D newFits(completeDataPath, cam->getFitsHeader());
                    newFits.setGaindb(task.getG());
                    double exptime = task.getE()/1000000.0;
                    newFits.setOntime(exptime);
                    newFits.setDateobs(frame.getAcqDateMicro());

                    vector<int> firstDateInt = TimeDate::getIntVectorFromDateString(task.getDate());
                    double  debObsInSeconds = firstDateInt.at(3)*3600 + firstDateInt.at(4)*60 + firstDateInt.at(5);
                    double  julianDate      = TimeDate::gregorianToJulian_2(firstDateInt);
                    double  julianCentury   = TimeDate::julianCentury(julianDate);
                    double  sideralT        = TimeDate::localSideralTime_2(julianCentury, firstDateInt.at(3), firstDateInt.at(4), firstDateInt.at(5), cam->getFitsHeader().getSitelong());
                    newFits.setCrval1(sideralT);

                    newFits.setCtype1("RA---ARC");
                    newFits.setCtype2("DEC--ARC");
                    newFits.setEquinox(2000.0);

                    string HHMMSS = Conversion::numbering(2, task.getH()) + Conversion::intToString(task.getH()) +
                                    Conversion::numbering(2, task.getM()) + Conversion::intToString(task.getM()) +
                                    Conversion::numbering(2, task.getS()) + Conversion::intToString(task.getS());

                    string fileName = "CAP_" + YYYYMMDD + "T" + HHMMSS + "_UT-" + Conversion::intToString(i);

                    switch(camFormat){

                        case MONO_8 :

                            {
                                // Create FITS image with BITPIX = BYTE_IMG (8-bits unsigned integers), pixel with TBYTE (8-bit unsigned byte)

                                if(newFits.writeFits(frame.getImg(), UC8, fileName))
                                    cout << ">> Fits saved in : " << completeDataPath << fileName << endl;

                            }

                            break;

                        case MONO_12 :

                            {

                                // Convert unsigned short type image in short type image.
                                Mat newMat = Mat(frame.getImg().rows, frame.getImg().cols, CV_16SC1, Scalar(0));

                                // Set bzero and bscale for print unsigned short value in soft visualization.
                                double bscale = 1;
                                double bzero  = 32768;
                                newFits.setBzero(bzero);
                                newFits.setBscale(bscale);

                                unsigned short * ptr;
                                short * ptr2;

                                for(int i = 0; i < frame.getImg().rows; i++){

                                    ptr = frame.getImg().ptr<unsigned short>(i);
                                    ptr2 = newMat.ptr<short>(i);

                                    for(int j = 0; j < frame.getImg().cols; j++){

                                        if(ptr[j] - 32768 > 32767){

                                            ptr2[j] = 32767;

                                        }else{

                                            ptr2[j] = ptr[j] - 32768;
                                        }
                                    }
                                }


                                // Create FITS image with BITPIX = SHORT_IMG (16-bits signed integers), pixel with TSHORT (signed short)
                                if(newFits.writeFits(newMat, S16, fileName))
                                    cout << ">> Fits saved in : " << completeDataPath << fileName << endl;

                            }
                    }
                }

            }



        }else{

            cout << endl << "Single capture failed !" << endl;

        }


    }

    cout<< "Restarting camera in continuous mode..." << endl;
    cam->acqRestart();

}

bool AcqThread::runRegularAcquisition(string frameDate){

    // Stop camera
    cout << "Stopping camera..." << endl;
    cam->acqStop();
	cam->grabStop();

    // If stack process exists.
    if(stackProcess != NULL && enableStackThread){

        boost::mutex::scoped_lock lock(*stackSignal_mutex);
        *stackSignal = false;
        lock.unlock();

        // Force interruption.
        cout << "Send interruption signal to stack " << endl;
        stackProcess->interruptThread();


    }

    // If detection process exists
    if(detectionProcess != NULL){

        boost::mutex::scoped_lock lock(*detSignal_mutex);
        *detSignal = false;
        lock.unlock();
        cout << "Send interruption signal to detection process " << endl;
        detectionProcess->interruptThread();

    }

    // Reset framebuffer.
    cout << "Cleaning frameBuffer..." << endl;
    boost::mutex::scoped_lock lock(*frameBuffer_mutex);
    frameBuffer->clear();
    lock.unlock();

    for(int i = 0; i < cam->getAcqRegularRepetition(); i++){

        // Configuration pour single capture
        Frame frame;
        cout << "Exposure : " << cam->getAcqRegularExposure() << endl;
        frame.setExposure(cam->getAcqRegularExposure());
        cout << "Gain : " << cam->getAcqRegularGain() << endl;
        frame.setGain(cam->getAcqRegularGain());
        cout << "Format : " << cam->getAcqRegularFormat() << endl;
        frame.setBitDepth(cam->getAcqRegularFormat());

        // Single capture.
        if(cam->grabSingleImage(frame, cam->getCameraId())){

            cout << endl << "Single capture succeed !" << endl;

            /// ---------------------- Save grabbed frame --------------------------

            // Save the frame in Fits 2D.
            if(frame.getImg().data){

                string	YYYYMMDD = TimeDate::get_YYYYMMDD_fromDateString(frameDate);
                cout << "YYYYMMDD : " << YYYYMMDD << endl;
                if(buildRegularAcquisitionDirectory(YYYYMMDD)){

                    cout << "Saving fits file ..." << endl;

                    cout << "completeDataPath : " << completeDataPath << endl;

                    Fits2D newFits(completeDataPath, cam->getFitsHeader());
                    newFits.setGaindb(cam->getAcqRegularGain());
                    double exptime = cam->getAcqRegularExposure()/1000000.0;
                    newFits.setOntime(exptime);
                    newFits.setDateobs(frame.getAcqDateMicro());

                    vector<int> firstDateInt = TimeDate::getIntVectorFromDateString(frameDate);
                    double  debObsInSeconds = firstDateInt.at(3)*3600 + firstDateInt.at(4)*60 + firstDateInt.at(5);
                    double  julianDate      = TimeDate::gregorianToJulian_2(firstDateInt);
                    double  julianCentury   = TimeDate::julianCentury(julianDate);
                    double  sideralT        = TimeDate::localSideralTime_2(julianCentury, firstDateInt.at(3), firstDateInt.at(4), firstDateInt.at(5), cam->getFitsHeader().getSitelong());
                    newFits.setCrval1(sideralT);

                    newFits.setCtype1("RA---ARC");
                    newFits.setCtype2("DEC--ARC");
                    newFits.setEquinox(2000.0);

                    string HHMMSS = Conversion::numbering(2, firstDateInt.at(3)) + Conversion::intToString(firstDateInt.at(3)) +
                                    Conversion::numbering(2, firstDateInt.at(4)) + Conversion::intToString(firstDateInt.at(4)) +
                                    Conversion::numbering(2, firstDateInt.at(5)) + Conversion::intToString(firstDateInt.at(5));

                    string fileName = "CAP_" + YYYYMMDD + "T" + HHMMSS + "_UT-" + Conversion::intToString(i);

                    cout << "fileName : " << fileName << endl;

                    switch(cam->getAcqRegularFormat()){

                        case MONO_8 :

                            {
                                // Create FITS image with BITPIX = BYTE_IMG (8-bits unsigned integers), pixel with TBYTE (8-bit unsigned byte)

                                if(newFits.writeFits(frame.getImg(), UC8, fileName))
                                    cout << ">> Fits saved in : " << completeDataPath << fileName << endl;

                            }

                            break;

                        case MONO_12 :

                            {

                                // Convert unsigned short type image in short type image.
                                Mat newMat = Mat(frame.getImg().rows, frame.getImg().cols, CV_16SC1, Scalar(0));

                                // Set bzero and bscale for print unsigned short value in soft visualization.
                                double bscale = 1;
                                double bzero  = 32768;
                                newFits.setBzero(bzero);
                                newFits.setBscale(bscale);

                                unsigned short * ptr;
                                short * ptr2;

                                for(int i = 0; i < frame.getImg().rows; i++){

                                    ptr = frame.getImg().ptr<unsigned short>(i);
                                    ptr2 = newMat.ptr<short>(i);

                                    for(int j = 0; j < frame.getImg().cols; j++){

                                        if(ptr[j] - 32768 > 32767){

                                            ptr2[j] = 32767;

                                        }else{

                                            ptr2[j] = ptr[j] - 32768;
                                        }
                                    }
                                }


                                // Create FITS image with BITPIX = SHORT_IMG (16-bits signed integers), pixel with TSHORT (signed short)
                                if(newFits.writeFits(newMat, S16, fileName))
                                    cout << ">> Fits saved in : " << completeDataPath << fileName << endl;


                            }
                    }
                }
            }

        }else{

            cout << endl << "Single capture failed !" << endl;

        }
    }

    cout<< "Restarting camera in continuous mode..." << endl;
    cam->acqRestart();

}


