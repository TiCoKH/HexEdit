// BGStats.cpp : statistics scan in background thread (part of CHexEditDoc)
//
// Copyright (c) 2010 by Andrew W. Phillips.
//
// No restrictions are placed on the noncommercial use of this code,
// as long as this text (from the above copyright notice to the
// disclaimer below) is preserved.
//
// This code may be redistributed as long as it remains unmodified
// and is not sold for profit without the author's written consent.
//
// This code, or any part of it, may not be used in any software that
// is sold for profit, without the author's written consent.
//
// DISCLAIMER: This file is provided "as is" with no expressed or
// implied warranty. The author accepts no liability for any damage
// or loss of business that this product may cause.
//

#include "stdafx.h"
#include "HexEdit.h"
#include "HexEditDoc.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// Returns true if global options says we can do background stats on this file
bool CHexEditDoc::CanDoStats()
{
	bool retval = false;
	if (theApp.bg_stats_)
	{
		retval = true;
		if (pfile1_ != NULL)
		{
			CString ss = pfile1_->GetFilePath().Left(3);
			if (IsDevice())
				retval = !theApp.bg_exclude_device_;
			else
				switch (::GetDriveType(ss))
				{
				case DRIVE_REMOVABLE:
					retval = !theApp.bg_exclude_removeable_;
					break;
				case DRIVE_REMOTE:
					retval = !theApp.bg_exclude_network_;
					break;
				case DRIVE_CDROM:
					retval = !theApp.bg_exclude_optical_;
					break;
				}
		}
	}
	return retval;
}

// Create/kill background thread as appropriate depending on current options
void CHexEditDoc::AlohaStats()
{
	if (pthread5_ == NULL && CanDoStats())  // no thread but we should have one
	{
		// create the stats thread
		CreateStatsThread();
		StartStats();
	}
	else if (pthread5_ != NULL && !CanDoStats())  // have thread but we shouldn't
	{
		KillStatsThread();
	}
}


void StartStats();
void StopStats();
UINT RunStatsThread();    // Main func in bg thread (needs to be public so it can be called from bg_func)

// Return how far our scan has progressed as a percentage (0 to 100).
int CHexEditDoc::StatsProgress()
{
	return 0;
}

// Stops the current background stats scan (if any).  It does not return 
// until the scan is aborted and the thread is waiting again.
void CHexEditDoc::StopStats()
{
	if (pthread5_ == NULL) return;

	bool waiting;
	docdata_.Lock();
	stats_command_ = STOP;
	docdata_.Unlock();
	SetThreadPriority(pthread5_->m_hThread, THREAD_PRIORITY_NORMAL);
	for (int ii = 0; ii < 100; ++ii)
	{
		// Wait just a little bit in case the thread was just about to go into wait state
		docdata_.Lock();
		waiting = stats_state_ == WAITING;
		docdata_.Unlock();
		if (waiting)
			break;
		TRACE("+++ StopStats - thread not waiting (yet)\n");
		Sleep(1);
	}
	SetThreadPriority(pthread5_->m_hThread, THREAD_PRIORITY_LOWEST);
	ASSERT(waiting);
}

// Start a new background scan.
void CHexEditDoc::StartStats()
{
	StopStats();

	// Setup up the info for the new scan
	docdata_.Lock();
	//stats_progress_ = 0;

	// Restart the scan
	stats_command_ = NONE;
	stats_fin_ = false;
	docdata_.Unlock();

	TRACE("+++ Pulsing stats event for %p\n", this);
	start_stats_event_.SetEvent();
}

// Kill background task and wait until it is dead
void CHexEditDoc::KillStatsThread()
{
	ASSERT(pthread5_ != NULL);
	if (pthread5_ == NULL) return;

	HANDLE hh = pthread5_->m_hThread;    // Save handle since it will be lost when thread is killed and object is destroyed
	TRACE1("+++ Killing stats thread for %p\n", this);

	// Signal thread to kill itself
	docdata_.Lock();
	stats_command_ = DIE;
	docdata_.Unlock();

	SetThreadPriority(pthread5_->m_hThread, THREAD_PRIORITY_NORMAL); // Make it a quick and painless death
	bool waiting, dying;
	for (int ii = 0; ii < 100; ++ii)
	{
		// Wait just a little bit in case the thread was just about to go into wait state
		docdata_.Lock();
		waiting = stats_state_ == WAITING;
		dying   = stats_state_ == DYING;
		docdata_.Unlock();
		if (waiting || dying)
			break;
		Sleep(1);
	}
	ASSERT(waiting || dying);

	// Send start message if it is on hold
	if (waiting)
		start_stats_event_.SetEvent();

	pthread5_ = NULL;
	DWORD wait_status = ::WaitForSingleObject(hh, INFINITE);
	ASSERT(wait_status == WAIT_OBJECT_0 || wait_status == WAIT_FAILED);

	// Free resources that are only needed during bg scan
	if (pfile5_ != NULL)
	{
		pfile5_->Close();
		delete pfile5_;
		pfile5_ = NULL;
	}
	for (int ii = 0; ii < doc_loc::max_data_files; ++ii)
	{
		if (data_file5_[ii] != NULL)
		{
			data_file5_[ii]->Close();
			delete data_file5_[ii];
			data_file5_[ii] = NULL;
		}
	}
}

static UINT bg_func(LPVOID pParam)
{
	CHexEditDoc *pDoc = (CHexEditDoc *)pParam;

	TRACE1("+++ Stats thread started for doc %p\n", pDoc);

	return pDoc->RunStatsThread();
}

void CHexEditDoc::CreateStatsThread()
{
	ASSERT(CanDoStats());
	ASSERT(pthread5_ == NULL);
	ASSERT(pfile5_ == NULL);

	// Open copy of file to be used by background thread
	if (pfile1_ != NULL)
	{
		if (IsDevice())
			pfile5_ = new CFileNC();
		else
			pfile5_ = new CFile64();
		if (!pfile5_->Open(pfile1_->GetFilePath(),
					CFile::modeRead|CFile::shareDenyNone|CFile::typeBinary) )
		{
			TRACE1("+++ File5 open failed for %p\n", this);
			return;
		}
	}

	// Open copy of any data files in use too
	for (int ii = 0; ii < doc_loc::max_data_files; ++ii)
	{
		ASSERT(data_file5_[ii] == NULL);
		if (data_file_[ii] != NULL)
			data_file5_[ii] = new CFile64(data_file_[ii]->GetFilePath(), 
										  CFile::modeRead|CFile::shareDenyWrite|CFile::typeBinary);
	}

	// Create new thread
	stats_command_ = NONE;
	stats_state_ = STARTING;
	stats_fin_ = false;
	TRACE1("+++ Creating stats thread for %p\n", this);
	pthread5_ = AfxBeginThread(&bg_func, this, THREAD_PRIORITY_LOWEST);
	ASSERT(pthread5_ != NULL);
}

// This is the main loop for the worker thread
UINT CHexEditDoc::RunStatsThread()
{
	// Keep looping until we get the kill signal
	for (;;)
	{
		{
			CSingleLock sl(&docdata_, TRUE);
			stats_state_ = WAITING;
		}
		TRACE1("+++ BGstats: waiting %p\n", this);
		DWORD wait_status = ::WaitForSingleObject(HANDLE(start_stats_event_), INFINITE);
		docdata_.Lock();
		stats_state_ = SCANNING;
		docdata_.Unlock();
		start_stats_event_.ResetEvent();      // Force ourselves to wait
		ASSERT(wait_status == WAIT_OBJECT_0);
		TRACE1("+++ BGstats: got event for %p\n", this);

		if (StatsProcessStop())
			continue;

		docdata_.Lock();
		stats_fin_ = false;
		FILE_ADDRESS file_len = length_;
		docdata_.Unlock();

		//stats_progress_ = 0;  xxx keep track of progress
		FILE_ADDRESS addr = 0;

		const size_t buf_size = 16384;
		ASSERT(stats_buf_ == NULL && c32_ == NULL && c64_ == NULL);
		stats_buf_ = new unsigned char[buf_size];

		if (file_len < LONG_MAX)
		{
			c32_ = new long[256];   // for small files only do 32-bit counts (faster)
			memset(c32_, '\0', 256*sizeof(*c32_));
		}
		else
		{
			c64_ = new __int64[256];
			memset(c64_, '\0', 256*sizeof(*c64_));
		}

		// Search all to_search_ blocks
		for (;;)
		{
			if (SearchProcessStop())
				break;

			size_t got;

			if ((got = GetData(stats_buf_, buf_size, addr, 5)) <= 0)
			{
				// We reached the end of the file at last - save results and go back to wait state
				TRACE("+++ BGState: finished scan for %p\n", this);
				CSingleLock sl(&docdata_, TRUE); // Protect shared data access

				if (c32_ != NULL)
				{
					for (int ii = 0; ii < 256; ++ii)
						count_[ii] = c32_[ii];
				}
				else
				{
					ASSERT(c64_ != NULL);
					for (int ii = 0; ii < 256; ++ii)
						count_[ii] = c64_[ii];
				}

				stats_fin_ = true;
				break;
			}

			if (c32_ != NULL)
			{
				for (size_t ii = 0; ii < got; ++ii)
				{
					++c32_[stats_buf_[ii]];
				}
			}
			else
			{
				ASSERT(c64_ != NULL);
				for (size_t ii = 0; ii < got; ++ii)
				{
					++c64_[stats_buf_[ii]];
				}
			}

			addr += got;
		} // for

		if (c32_ != NULL) (delete[] c32_), c32_ = NULL;
		if (c64_ != NULL) (delete[] c64_), c64_ = NULL;

		delete[] stats_buf_;
		stats_buf_ = NULL;
	}
	return 0;  // never reached
}

bool CHexEditDoc::StatsProcessStop()
{
	bool retval = false;

	CSingleLock sl(&docdata_, TRUE);
	switch (stats_command_)
	{
	case STOP:                      // stop scan and wait
		TRACE1("+++ BGstats: stop for %p\n", this);
		retval = true;
		break;
	case DIE:                       // terminate this thread
		TRACE1("+++ BGstats: killed thread for %p\n", this);
		stats_state_ = DYING;
		sl.Unlock();                // we need this here as AfxEndThread() never returns so d'tor is not called
		delete[] stats_buf_;
		stats_buf_ = NULL;
		if (c32_ != NULL) (delete[] c32_), c32_ = NULL;
		if (c64_ != NULL) (delete[] c64_), c64_ = NULL;
		AfxEndThread(1);            // kills thread (no return)
		break;                      // Avoid warning
	case NONE:                      // nothing needed here - just continue scanning
		break;
	default:                        // should not happen
		ASSERT(0);
	}

	stats_command_ = NONE;
	return retval;
}