#include "stdafx.h"
#include "Tracker.h"
#include <algorithm>
#include "Matrix33.h"

extern HANDLE g_TStateMutex;
extern TState g_TState;

DWORD WINAPI TrackerPollingThreadFunc (LPVOID lpParam);
double CaliPosition[3];
int TCounter = 0;
Tracker::Tracker(void)
{	
	hTrackerPollingThread = INVALID_HANDLE_VALUE;
	updateInterval = 200-200; //200ms default
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	cpuFreq = freq.QuadPart;
}

Tracker::~Tracker(void)
{
	this->Close();
}
int Tracker::Ini(void)
{
	int iResult = 0;

	bool TransmitMulticast = false;
	std::string HostName = "147.46.175.54:801";//"localhost:801";

	
  // Connect to a server
  std::cout << "Connecting to " << HostName << " ..." << std::flush;
  while( !MyClient.IsConnected().Connected )
  {
    // Direct connection
    MyClient.Connect( HostName );
    std::cout << ".";
    Sleep( 200 );
  }
  std::cout<<std::endl;
  MyClient.EnableSegmentData();
  MyClient.EnableMarkerData();
  MyClient.EnableUnlabeledMarkerData();
  MyClient.EnableDeviceData();

  std::cout << "Segment Data Enabled: "          << Adapt( MyClient.IsSegmentDataEnabled().Enabled )         << std::endl;
  std::cout << "Marker Data Enabled: "           << Adapt( MyClient.IsMarkerDataEnabled().Enabled )          << std::endl;
  std::cout << "Unlabeled Marker Data Enabled: " << Adapt( MyClient.IsUnlabeledMarkerDataEnabled().Enabled ) << std::endl;
  std::cout << "Device Data Enabled: "           << Adapt( MyClient.IsDeviceDataEnabled().Enabled )          << std::endl;

  // Set the streaming mode
  MyClient.SetStreamMode( ViconDataStreamSDK::CPP::StreamMode::ClientPull );
  
  MyClient.SetAxisMapping(	Direction::Forward, 
							Direction::Right, 
							Direction::Down ); // Z-down
  Output_GetAxisMapping _Output_GetAxisMapping = MyClient.GetAxisMapping();
  std::cout << "Axis Mapping: X-" << Adapt( _Output_GetAxisMapping.XAxis ) 
                         << " Y-" << Adapt( _Output_GetAxisMapping.YAxis ) 
                         << " Z-" << Adapt( _Output_GetAxisMapping.ZAxis ) << std::endl;

  // Discover the version number
  Output_GetVersion _Output_GetVersion = MyClient.GetVersion();
  std::cout << "Version: " << _Output_GetVersion.Major << "." 
                           << _Output_GetVersion.Minor << "." 
                           << _Output_GetVersion.Point << std::endl;

  if( TransmitMulticast )
  {
    MyClient.StartTransmittingMulticast( "localhost", "224.0.0.0" );
  }

  // Here Start the Polling loop
	hTrackerPollingThread = CreateThread(NULL, 0, TrackerPollingThreadFunc, this, 0, NULL);
  return iResult;
}

int Tracker::Close(void)
{
	int iResult = 0;
	if(hTrackerPollingThread != INVALID_HANDLE_VALUE)
	CloseHandle(hTrackerPollingThread);
	MyClient.Disconnect();
	return iResult;

}

bool Less(TState item1, TState item2)
{
	return item1.time_to_go < item2.time_to_go;
}

DWORD WINAPI TrackerPollingThreadFunc (LPVOID lpParam)  // The rate is about 100Hz
{
	Tracker *pTracker = (Tracker *)lpParam;

	TState last_TState;
	last_TState.FrameNumber = pTracker->MyClient.GetFrameNumber().FrameNumber;
	last_TState.TotalLatency = pTracker->MyClient.GetLatencyTotal().Total;
	last_TState.NoOfSubjects = pTracker->MyClient.GetSubjectCount().SubjectCount;
	std::string SubjectName = pTracker->MyClient.GetSubjectName(0).SubjectName;
	last_TState.NoOfSegments = pTracker->MyClient.GetSegmentCount(SubjectName).SegmentCount;
	std::string SegmentName = pTracker->MyClient.GetSegmentName(SubjectName,0).SegmentName;

	while(true)
	{
		while( pTracker->MyClient.GetFrame().Result != Result::Success )
		{
		  // Sleep a little so that we don't lumber the CPU with a busy poll
			Sleep( pTracker->updateInterval );
		}
		
	TState cur_TState;
	cur_TState.FrameNumber = pTracker->MyClient.GetFrameNumber().FrameNumber;
	cur_TState.TotalLatency = pTracker->MyClient.GetLatencyTotal().Total;
	cur_TState.NoOfSubjects = pTracker->MyClient.GetSubjectCount().SubjectCount;
	std::string SubjectName = pTracker->MyClient.GetSubjectName(0).SubjectName;
	cur_TState.NoOfSegments = pTracker->MyClient.GetSegmentCount(SubjectName).SegmentCount;
	std::string SegmentName = pTracker->MyClient.GetSegmentName(SubjectName,0).SegmentName;

	
	//Here calculate velocity

	static LARGE_INTEGER l;
	static LARGE_INTEGER llast = {0};
	
	QueryPerformanceCounter(&l);
	cur_TState.seqNo = l.QuadPart;
	double freq = (double)pTracker->cpuFreq;

	double T = (double)((l.QuadPart)-(llast.QuadPart))/freq;
	llast.QuadPart = l.QuadPart;
	
	//printf("The time interval %f\r",T);
	static double xlast[3]  = {0};
	static double vlast[3] = {0};
	
	memcpy(cur_TState.Translation, pTracker->MyClient.GetSegmentGlobalTranslation( SubjectName, SegmentName ).Translation, sizeof(cur_TState.Translation)); // position
	memcpy(cur_TState.RotationMatrix, pTracker->MyClient.GetSegmentGlobalRotationMatrix( SubjectName, SegmentName ).Rotation, sizeof(cur_TState.RotationMatrix)); // rotation matrix
	memcpy(cur_TState.RotationEuler, pTracker->MyClient.GetSegmentGlobalRotationEulerXYZ(SubjectName, SegmentName ).Rotation, sizeof(cur_TState.RotationEuler)); //euler angle
	//Euler Sequence and dimension of euler angles
	static double xlast2[3]  = {0};
	static double vlast2[3] = {0};

	//velocity
	double Wlast = 0.5;
	cur_TState.Velocity[0] = (1.0-Wlast)*(cur_TState.Translation[0] - xlast[0])/T + Wlast*vlast[0]; 
	cur_TState.Velocity[1] = (1.0-Wlast)*(cur_TState.Translation[1] - xlast[1])/T + Wlast*vlast[1];
	cur_TState.Velocity[2] = (1.0-Wlast)*(cur_TState.Translation[2] - xlast[2])/T + Wlast*vlast[2];

	//angular velocity
	Matrix33 R1(cur_TState.RotationMatrix);// R is from Body frame to Global Frame
	static Matrix33 R1_last(R1);

	static double a_unit[9] = {1.0,0.0,0.0,
	0.0,1.0,0.0,
	0.0,0.0,1.0};
	
	Matrix33 R_dot(a_unit);
	R_dot = (R1 + R1_last*(-1)) / T;
	Matrix33 Sw1_raw = R1.Trans()*R_dot;

	double Sw_lpf=0.999;
	static Matrix33 Sw1_last = Sw1_raw;
	Matrix33 Sw1 = Sw1_raw*(1 - Sw_lpf) + Sw1_last*Sw_lpf;
	
	Sw1_last = Sw1;
	R1_last = R1;
	Vector3 w1(Sw1.r3.y, Sw1.r1.z, Sw1.r2.x);

	cur_TState.AngularVelocity[0] = w1.x;
	cur_TState.AngularVelocity[1] = w1.y;
	cur_TState.AngularVelocity[2] = w1.z;

	memcpy(xlast, cur_TState.Translation, sizeof(xlast));
	memcpy(vlast, cur_TState.Velocity, sizeof(vlast));
	

	// copying TState from cur_TState to g_TState
	WaitForSingleObject(g_TStateMutex, INFINITE);
	memcpy(&g_TState, &cur_TState, sizeof(TState));
	ReleaseMutex(g_TStateMutex);

	
	}//while(true)
}