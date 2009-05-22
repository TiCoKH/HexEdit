// BGAerial.cpp : background scan of file to create aerial view "bitmap"
//
// Copyright (c) 2008 by Andrew W. Phillips.
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

/*
Aerial View Scanning
====================

A background thread is created to scan the file to build up a bitmap for
display in an "aerial view" which can show an alternative view to the
normal hex view of a document.

Note This background search may later also be used to generate stats
on the file such as byte counts.

Document
--------

A background thread is created for every document that has at least
one aerial view.  It has a low priority (LOWEST?).  Communication with
the thread is achieved using these document members:

start_aerial_event_: signals the thread it needs to do something when in wait state
aerial_state_: updated by the thread to say what it's doing: waiting, scanning or dying
aerial_command_: command to the thread to tell it what to do: scan, stop, die
docdata_: a critical section to protect access to shared document members

pfile3_: is a ptr to file open the same as pfile1_.  Using a separate file allows the main thread
         to read from the file without having to lock docdata_.  Locking is only required
         when the background thread accesses the file or the main thread changes it.
         This means that file display should never be slowed by the background thread.
loc_: accessed (via GetData) in main/search/aerial threads to get data from the file
undo_: loc_ uses data stored in undo array

Background thread
-----------------

The thread has 3 states: waiting, scanning and dead.  When waiting it is blocked
by the event (start_aerial_event_), and to change the state the event needs to be
pulsed (and a command given).  After it is unblocked it goes straight into
scanning state but then immediately check for a new command.

While scanning it regularly checks for a new command (aerial_command_) while in its
processing loop.  If it detects a "stop" command it goes back into the wait state.
If it detects a "restart" command it continues scanning but start processing of the
whole file.  If it detects a "die" command the thread terminates itself.

After detecting a new command the thread clears the command (sets it to "none") so that
it does not process it again.

After the thread scans the whole file it goes back into the wait state.
But first it signals the main thread which is passed on to the document
and thence to all aerial views so they can update themselves.
Note that if a modal dialog is active when the ::PostThreadMessage is called
to send the message to the main thread then the message is lost.


Changes (CHexEditDoc::Change, CHexEditDoc::Undo in DocData.cpp)
-------

When a document is changed the scan needs to be restarted.

This may later be changed to allow small document changes to update
the bitmap without having to rescan the whole thing.  This would work
fine for replacements.  For deletions a memove would be required.
For small insertions only a memove may be required but it may be that
the bitmap has to be resized, which may be handled by realloc or
just a compete re-scan.

   
Views (see CBGSearchHint used by CHexEditView::OnUpdate)
-----

When the colour scheme of the view used to generate the aerial view
is changed then the file has to be rescanned as the bitmap
colours may be completely different.  This is complicated by the 
fact that a document may have more than one view using different
colour schemes.

*/

#include "stdafx.h"
#include "HexEdit.h"
#include <memory.h>

#include "HexEditDoc.h"
#include "HexEditView.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// bg_func is the entry point for the thread.
// It just calls the RunAerialThread member for the doc passed to it.
static UINT bg_func(LPVOID pParam)
{
    CHexEditDoc *pDoc = (CHexEditDoc *)pParam;

    TRACE1("Aerial thread started for doc %p\n", pDoc);

    return pDoc->RunAerialThread();
}

// Add an aerial view for this doc.  The doc has to remember how many there are so it
// can free up things when there are no more.  (There can be more than one if a 2nd
// window has been opened on the same document.)
// It has to know the associated CHexEditView in order to know the colour scheme and
// hence the colours assigned in the FreeImage bitmap.

void CHexEditDoc::AddAerialView(CHexEditView *pview)
{
    if (++av_count_ == 1)
    {
        pview->get_colours(kala_);

        // xxx we need user options for default bpe and MAX_BMP (min MAX_BMP should be 16MB)
        bpe_ = 1; 
        
        // Keep increasing bpe_ by powers of two until we get a small enough bitmap
        while (bpe_ <= 65536 && (length_*3)/bpe_ > MAX_BMP)
            bpe_ = bpe_<<1;

        // Work out the number of bitmap rows we would need at the widest bitmap size
        int rows = int(length_/bpe_/MAX_WIDTH) + 2;    // Round up to next row plus add one more row to allow for "reshaping"
        ASSERT((rows-2)*MAX_WIDTH < MAX_BMP);

        dib_ = FreeImage_Allocate(MAX_WIDTH, rows, 24);
        dib_size_ = MAX_WIDTH*rows*3;           // DIB size in bytes since we have 3 bytes per pixel and no pad bytes at the end of each scan line
        ASSERT(dib_size_ == FreeImage_GetPitch(dib_) * FreeImage_GetHeight(dib_));
        memset(FreeImage_GetBits(dib_), 0xC0, dib_size_);       // Clear to light grey
#ifdef TEST_AERIAL
        // Fill the bitmap with a known pattern for testing of scrolling etc
        BYTE *pp = FreeImage_GetBits(dib_);
        for (int f4 = 0; ; ++f4)
        {
            for (int f3 = 0; f3 < 32; ++f3)
            {
                for (int f2 = 0; f2 < 32; ++f2)
                {
                    for (int f1 = 0; f1 < 32; ++f1)
                    {
                        if ((((f4*32 + f3)*32 + f2)*32 + f1)*32 > NumElts())
                            goto stop_now;
                        *(pp + ((((f4*32) + f3)*32 + f2)*32 + f1)*32*3) = (BYTE)255;
                        *(pp + ((((f4*32) + f3)*32 + f2)*32 + f1)*32*3 + 1) = (BYTE)0;
                        *(pp + ((((f4*32) + f3)*32 + f2)*32 + f1)*32*3 + 2) = (BYTE)0;
                    }
                    *(pp + ((f4*32 + f3)*32 + f2)*32*32*3) = (BYTE)0;
                    *(pp + ((f4*32 + f3)*32 + f2)*32*32*3 + 1) = (BYTE)255;
                    *(pp + ((f4*32 + f3)*32 + f2)*32*32*3 + 2) = (BYTE)0;
                    *(pp + ((f4*32 + f3)*32 + f2)*32*32*3 + 3) = (BYTE)0;
                    *(pp + ((f4*32 + f3)*32 + f2)*32*32*3 + 4) = (BYTE)255;
                    *(pp + ((f4*32 + f3)*32 + f2)*32*32*3 + 5) = (BYTE)0;
                }
                *(pp + (f4*32 + f3)*32*32*32*3) = (BYTE)0;
                *(pp + (f4*32 + f3)*32*32*32*3 + 1) = (BYTE)0;
                *(pp + (f4*32 + f3)*32*32*32*3 + 2) = (BYTE)255;
                *(pp + (f4*32 + f3)*32*32*32*3 + 3) = (BYTE)0;
                *(pp + (f4*32 + f3)*32*32*32*3 + 4) = (BYTE)0;
                *(pp + (f4*32 + f3)*32*32*32*3 + 5) = (BYTE)255;
                *(pp + (f4*32 + f3)*32*32*32*3 + 6) = (BYTE)0;
                *(pp + (f4*32 + f3)*32*32*32*3 + 7) = (BYTE)0;
                *(pp + (f4*32 + f3)*32*32*32*3 + 8) = (BYTE)255;
            }
            memset(pp + f4*32*32*32*32*3, 255, 30);  // 10 white pixels
        }
    stop_now:
        memset(FreeImage_GetBits(dib_), 0, 6);                    // 2 black pixels at the start
        memset(FreeImage_GetBits(dib_) + NumElts()*3 - 6, 0, 6);  // 2 black pixels at the end
#else
        CreateAerialThread();
        TRACE("Pulsing aerial event\n");
        start_aerial_event_.SetEvent();
#endif
    }
#ifndef TEST_AERIAL
    ASSERT(pthread3_ != NULL);
#endif
}

void CHexEditDoc::RemoveAerialView()
{
    if (--av_count_ == 0)
    {
        if (pthread3_ != NULL)
            KillAerialThread();
        FIBITMAP *dib = dib_;
        dib_ = NULL;
        FreeImage_Unload(dib);
    }
}

void CHexEditDoc::CreateAerialThread()
{
    CHexEditApp *aa = dynamic_cast<CHexEditApp *>(AfxGetApp());
    ASSERT(pthread3_ == NULL);
    ASSERT(pfile3_ == NULL);

    aerial_fin_ = FALSE;

    // Open copy of file to be used by background thread
    if (pfile1_ != NULL)
	{
		if (IsDevice())
			pfile3_ = new CFileNC();
		else
			pfile3_ = new CFile64();
		if (!pfile3_->Open(pfile1_->GetFilePath(),
					CFile::modeRead|CFile::shareDenyNone|CFile::typeBinary) )
		{
			TRACE1("Aerial file open failed for %p\n", this);
			return;
		}
	}

	// Open copy of any data files in use too
	for (int ii = 0; ii < doc_loc::max_data_files; ++ii)
	{
		ASSERT(data_file3_[ii] == NULL);
		if (data_file_[ii] != NULL)
			data_file2_[ii] = new CFile64(data_file_[ii]->GetFilePath(), 
										  CFile::modeRead|CFile::shareDenyWrite|CFile::typeBinary);
	}

    // Create new thread
    TRACE1("Creating aerial thread for %p\n", this);
    aerial_command_ = RESTART;
    pthread3_ = AfxBeginThread(&bg_func, this, THREAD_PRIORITY_LOWEST);
    ASSERT(pthread3_ != NULL);
}

void CHexEditDoc::KillAerialThread()
{
    ASSERT(pthread3_ != NULL);
    if (pthread3_ == NULL) return;

    HANDLE hh = pthread3_->m_hThread;    // Save handle since it will be lost when thread is killed and object is destroyed
    TRACE1("Killing aerial thread for %p\n", this);

    // Signal thread to kill itself
    docdata_.Lock();
    bool waiting = aerial_state_ == WAITING;
    aerial_command_ = DIE;
    docdata_.Unlock();

    SetThreadPriority(pthread3_->m_hThread, THREAD_PRIORITY_NORMAL); // Make it a quick and painless death

	if (!waiting)
	{
		// Wait just a little bit in case the thread was just about to go into wait state
		Sleep(500);
		docdata_.Lock();
		bool waiting = aerial_state_ == WAITING;
		docdata_.Unlock();
	}

    timer tt(true);
    if (waiting)
        start_aerial_event_.SetEvent();

    DWORD wait_status = ::WaitForSingleObject(hh, INFINITE);
    ASSERT(wait_status == WAIT_OBJECT_0);
    tt.stop();
    TRACE1("Thread took %g secs to kill\n", double(tt.elapsed()));
    pthread3_ = NULL;

    // Free resources that are only needed during bg searches
    if (pfile3_ != NULL)
	{
		pfile3_->Close();
		delete pfile3_;
		pfile3_ = NULL;
	}
	for (int ii = 0; ii < doc_loc::max_data_files; ++ii)
	{
		if (data_file3_[ii] != NULL)
		{
			data_file3_[ii]->Close();
			delete data_file3_[ii];
			data_file3_[ii] = NULL;
		}
	}
	// xxx reset any other things used by the thread
}

// Doc has changed - restart scan
void CHexEditDoc::AerialChange()
{
    if (av_count_ == 0) return;
    ASSERT(pthread3_ != NULL);
    if (pthread3_ == NULL) return;

	docdata_.Lock();
	bool waiting = aerial_state_ == WAITING;
	aerial_command_ = RESTART;
	aerial_fin_ = FALSE;
	docdata_.Unlock();

	if (!waiting)
	{
		// Wait just a little bit in case the thread was just about to go into wait state
	    SetThreadPriority(pthread3_->m_hThread, THREAD_PRIORITY_NORMAL);
		Sleep(0);
	    SetThreadPriority(pthread3_->m_hThread, THREAD_PRIORITY_LOWEST);
		docdata_.Lock();
		bool waiting = aerial_state_ == WAITING;
		docdata_.Unlock();
	}

    if (waiting)
        start_aerial_event_.SetEvent();
}

bool CHexEditDoc::AerialScanning()
{
    docdata_.Lock();
    bool waiting = aerial_state_ == WAITING;
    docdata_.Unlock();

    return !waiting;
}

// This is what does the work in the background thread
UINT CHexEditDoc::RunAerialThread()
{
    // Keep looping until we are told to die
    for (;;)
    {
        // Signal that we are wait then wait for start_aerial_event_ to be pulsed
        {
            CSingleLock sl(&docdata_, TRUE);
            aerial_state_ = WAITING;
        }
        TRACE1("BGAerial: waiting for %p\n", this);
        DWORD wait_status = ::WaitForSingleObject(HANDLE(start_aerial_event_), INFINITE);
        start_aerial_event_.ResetEvent();
        ASSERT(wait_status == WAIT_OBJECT_0);
        TRACE1("BGAerial: got event for %p\n", this);

        FILE_ADDRESS addr = 0;
        docdata_.Lock();
        FILE_ADDRESS file_len = length_;
        int file_bpe = bpe_;
        unsigned char *file_dib = FreeImage_GetBits(dib_);
        ASSERT(aerial_command_ == RESTART || aerial_command_ == DIE);   // we should only be woken up to scan or die
        docdata_.Unlock();

        // Get the file buffer
        size_t buf_len = (size_t)min(file_len, 65536);
        unsigned char *buf = new unsigned char[buf_len];

        for (;;)
        {
             // First check what we need to do
            {
                CSingleLock sl(&docdata_, TRUE);
                switch(aerial_command_)
                {
                case RESTART:                   // restart scan from beginning
                    aerial_command_ = NONE;
                    TRACE1("BGAerial: restart for %p\n", this);
                    addr = 0;
                    file_len = length_;
                    file_bpe = bpe_;
                    aerial_state_ = SCANNING;
                    break;
                case STOP:                      // stop scan and wait
                    aerial_command_ = NONE;
                    TRACE1("BGAerial: stop for %p\n", this);
                    aerial_state_ = WAITING;
                    goto end_scan;
                case DIE:                       // terminate this thread
                    aerial_command_ = NONE;
                    TRACE1("BGAerial: killed thread for %p\n", this);
                    aerial_state_ = DYING;
                    return 1;
                case NONE:                      // nothing needed here - just continue scanning
                    ASSERT(aerial_state_ == SCANNING);
                    break;
                default:                        // should not happen
                    ASSERT(0);
                }
            }

            // Check if we have finished scanning the file
            if (addr >= file_len)
            {
                TRACE1("BGAerial: finished scan for %p\n", this);
                CSingleLock sl(&docdata_, TRUE); // Protect shared data access

                aerial_fin_ = TRUE;
                break;                          // falls out to end_scan
            }

            // Get the next buffer full from the file and scan it
            size_t got = GetData(buf, buf_len, addr, 3);
            ASSERT(got <= buf_len);

            unsigned char *pbm = file_dib + 3*size_t(addr/file_bpe);    // where we write to bitmap
            unsigned char *pbuf;                                        // where we read from the file buffer
            for (pbuf = buf; pbuf < buf + got; pbuf += file_bpe, pbm += 3)
            {
                int r, g, b;
                r = g = b = 0;
                for (unsigned char *pp = pbuf; pp < pbuf + file_bpe; ++pp)
                {
                    r += GetRValue(kala_[*pp]);
                    g += GetGValue(kala_[*pp]);
                    b += GetBValue(kala_[*pp]);
                }
                *pbm     = unsigned char(b/file_bpe);
                *(pbm+1) = unsigned char(g/file_bpe);
                *(pbm+2) = unsigned char(r/file_bpe);
            }
            addr += got;
        }
    end_scan:
        ;
    }
    return 1;
}