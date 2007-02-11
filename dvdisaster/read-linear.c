/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2007 Carsten Gnoerlich.
 *  Project home page: http://www.dvdisaster.com
 *  Email: carsten@dvdisaster.com  -or-  cgnoerlich@fsfe.org
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA,
 *  or direct your browser at http://www.gnu.org.
 */

#include "dvdisaster.h"

#include "scsi-layer.h"

/***
 *** Local data package used during reading 
 ***/

#define READ_BUFFERS 128   /* equals 4MB of buffer space */

enum { BUF_EMPTY, BUF_FULL, BUF_DEAD, BUF_EOF };

typedef struct
{  LargeFile *readerImage;  /* we need two file handles to prevent LargeSeek() */
   LargeFile *writerImage;  /* race conditions between the reader and writer */
   struct _DeviceHandle *dh;
   EccInfo *ei;
   GThread *worker;
   struct MD5Context md5ctxt;

   /* Data exchange between reader and worker */

   AlignedBuffer *alignedBuf[READ_BUFFERS];
   gint64 bufferedSector[READ_BUFFERS];
   int nSectors[READ_BUFFERS];
   int bufState[READ_BUFFERS];
   GMutex *mutex;
   GCond *canRead, *canWrite;
   int readPtr,writePtr;
   char *workerError;

   /* for usage within the reader */

   gint64 sectors;                   /* medium capacity */
   gint64 firstSector, lastSector;   /* reading range */
   gint64 readMarker;
   int rereading;                    /* TRUE if working on existing image */
   char *msg;
   GTimer *speedTimer,*readTimer;
   int unreportedError;
   int earlyTermination;
   int scanMode;
   gint64 readOK;
   gint64 deadWritten;
   int pass;
} read_closure;

/*
 * Send EOF to the worker thread
 */

static void send_eof(read_closure *rc)
{
   g_mutex_lock(rc->mutex);
   while(rc->bufState[rc->readPtr] != BUF_EMPTY)
     g_cond_wait(rc->canRead, rc->mutex);

   rc->bufState[rc->readPtr] = BUF_EOF;
   rc->readPtr++;
   if(rc->readPtr >= READ_BUFFERS)
     rc->readPtr = 0;

   g_cond_signal(rc->canWrite);
   g_mutex_unlock(rc->mutex);
}

/*
 * Cleanup. 
 */

static void cleanup(gpointer data)
{  read_closure *rc = (read_closure*)data;
   int full_read = FALSE;
   int aborted   = rc->earlyTermination;
   int scan_mode = rc->scanMode;
   int i;

   /* Reset temporary ignoring of fatal errors.
      User has to set this in the preferences to make it permanent. */

   if(Closure->ignoreFatalSense == 2)
      Closure->ignoreFatalSense = 0;

   /* This is a failure condition */

   if(g_thread_self() == rc->worker)
   {  g_printf("Reading/Scanning terminated from worker thread - trouble ahead\n");
      return;
   }

   /* Make sure worker thread exits gracefully */

   if(rc->worker && !rc->workerError)
   {  send_eof(rc);
      g_thread_join(rc->worker);
   }

   /* Clean up reader thread */

   if(rc->dh)
     full_read = (rc->readOK == rc->dh->sectors && !Closure->crcErrors);

   Closure->cleanupProc = NULL;

   if(Closure->guiMode)
   {  if(rc->unreportedError)
         SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
			      _("<span %s>Aborted by unrecoverable error.</span> %lld sectors read, %lld sectors unreadable/skipped so far."),
			      Closure->redMarkup, rc->readOK, Closure->readErrors); 
   }

   if(rc->readerImage)   
     if(!LargeClose(rc->readerImage))
       Stop(_("Error closing image file:\n%s"), strerror(errno));
   if(rc->writerImage)   
     if(!LargeClose(rc->writerImage))
       Stop(_("Error closing image file:\n%s"), strerror(errno));
   if(rc->dh)      CloseDevice(rc->dh);
   if(rc->ei)      FreeEccInfo(rc->ei);

   if(rc->mutex)    g_mutex_free(rc->mutex);
   if(rc->canRead)  g_cond_free(rc->canRead);
   if(rc->canWrite) g_cond_free(rc->canWrite);
   if(rc->workerError) g_free(rc->workerError);

   for(i=0; i<READ_BUFFERS; i++)
     if(rc->alignedBuf[i])
       FreeAlignedBuffer(rc->alignedBuf[i]);

   if(rc->msg)     g_free(rc->msg);
   if(rc->speedTimer) g_timer_destroy(rc->speedTimer);
   if(rc->readTimer)  g_timer_destroy(rc->readTimer);
   g_free(rc);

   if(Closure->readAndCreate && Closure->guiMode && !strncmp(Closure->methodName, "RS01", 4)
      && !scan_mode && !aborted)
   {  if(!full_read)
      {  ModalDialog(GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, NULL,
		     _("Automatic error correction file creation\n"
		       "is only possible after a full reading pass.\n"));
	 AllowActions(TRUE);
      }
      else ContinueWithAction(ACTION_CREATE_CONT); 
   }
   else 
     if(Closure->guiMode)
       AllowActions(TRUE);

   if(!full_read && Closure->crcCache)
     ClearCrcCache();

   if(scan_mode)   /* we haven't created an image, so throw away the crc sums */
     ClearCrcCache();

   g_thread_exit(0);
}

/***
 *** Helper functions for the reader
 ***/

/*
 * Register with different label texts depending on rc->scanMode
 */

static void register_reader(read_closure *rc)
{
   if(rc->scanMode)  /* Output messages differ in read and scan mode */
   {  RegisterCleanup(_("Scanning aborted"), cleanup, rc);
      if(Closure->guiMode)
	SetLabelText(GTK_LABEL(Closure->readLinearHeadline), 
		     "<big>%s</big>\n<i>%s</i>",
		     _("Scanning medium for read errors."),
		     _("Medium: not yet determined"));
   }
   else
   {  RegisterCleanup(_("Reading aborted"), cleanup, rc);
      if(Closure->guiMode)
       SetLabelText(GTK_LABEL(Closure->readLinearHeadline), 
		    "<big>%s</big>\n<i>%s</i>",
		    _("Preparing for reading the medium image."),
		    _("Medium: not yet determined"));
   }
}

/* 
 * If ecc file exists and automatic ecc creation is enabled,
 * ask user if we may remove the existing one. 
 */

static void confirm_ecc_file_deletion(read_closure *rc)
{
   if(Closure->readAndCreate && !rc->scanMode)
   {  gint64 ignore;

      if(LargeStat(Closure->eccName, &ignore))
      {  if(Closure->guiMode)
	 {  int answer = ModalDialog(GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, NULL,
				    _("Automatic error correction file creation is enabled,\n"
				      "and \"%s\" already exists.\n"
				      "Overwrite it?\n"),
				    Closure->eccName);

	    if(!answer)
	    {  SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
				    _("<span %s>Aborted by user request!</span>"), 
				    Closure->redMarkup); 
	       rc->unreportedError = FALSE;
	       cleanup((gpointer)rc);
	    }
	 }
      }
   }
}

/*
 * See if we have an ecc file which belongs to the medium 
 */

static void check_ecc_file(read_closure *rc)
{
  rc->ei = OpenEccFile(READABLE_ECC | PRINT_MODE);

  /* Compare the fingerprint sectors */

  if(rc->ei) 
  {  guint8 fingerprint[16];
     int fp_read;

     fp_read = GetMediumFingerprint(rc->dh, fingerprint, rc->ei->eh->fpSector);

     if(fp_read && !memcmp(fingerprint, rc->ei->eh->mediumFP, 16))
	Closure->checkCrc = TRUE;
     else
     {  Closure->checkCrc = FALSE;
	FreeEccInfo(rc->ei);
	rc->ei = NULL;
     }
  }
}

/*
 * Find out current which mode we are operatin in:
 * 1. Scanning
 * 2. Creating a new image
 * 3. Completing an existing image
 * Output respective messages and prepare the image file.
 */

static void determine_mode(read_closure *rc)
{  guint8 medium_fp[16], image_fp[16];
   gint64 image_size;
   unsigned char *buf = rc->alignedBuf[0]->buf;
   int unknown_fingerprint = FALSE;
   char *t;

   /*** In scan mode we simply need to output some messages. */

   if(rc->scanMode)
   {  
      rc->msg = g_strdup(_("Scanning medium for read errors."));

      PrintLog("%s\n", rc->msg);
      if(Closure->guiMode)
      {  if(Closure->checkCrc)
	   SetLabelText(GTK_LABEL(Closure->readLinearHeadline), 
			"<big>%s</big>\n<i>- %s -</i>", rc->msg,
			_("Reading CRC information from ecc file"));

         else
	   SetLabelText(GTK_LABEL(Closure->readLinearHeadline), 
			"<big>%s</big>\n<i>%s</i>", rc->msg, rc->dh->mediumDescr);
      }

      rc->readMarker = 0;

      if(Closure->guiMode)
	 InitializeCurve(rc->dh->maxRate, rc->firstSector, rc->lastSector, rc->sectors);

      return;
   } 

   /*** If no image file exists, open a new one. */

reopen_image:
   if(!LargeStat(Closure->imageName, &image_size))
   {  
      rc->msg = g_strdup(_("Reading new medium image."));
      
      if(!(rc->writerImage = LargeOpen(Closure->imageName, O_WRONLY | O_CREAT, IMG_PERMS)))
	 Stop(_("Can't open %s:\n%s"),Closure->imageName,strerror(errno));
      if(!(rc->readerImage = LargeOpen(Closure->imageName, O_RDONLY, IMG_PERMS)))
	 Stop(_("Can't open %s:\n%s"),Closure->imageName,strerror(errno));

      PrintLog(_("Creating new %s image.\n"),Closure->imageName);
      if(Closure->guiMode)
      {  if(Closure->checkCrc)
	    SetLabelText(GTK_LABEL(Closure->readLinearHeadline),
			 "<big>%s</big>\n<i>%s</i>", rc->msg,
			 _("Reading CRC information from ecc file"));
	 else
	    SetLabelText(GTK_LABEL(Closure->readLinearHeadline),
			 "<big>%s</big>\n<i>%s</i>", rc->msg, rc->dh->mediumDescr);
      }
      rc->rereading  = FALSE;
      rc->readMarker = 0;

      if(Closure->guiMode)
	 InitializeCurve(rc->dh->maxRate, rc->firstSector, rc->lastSector, rc->sectors);

      return;
   }

   /*** Examine the given image file */

   t = _("Completing existing medium image.");

   /* Use the existing file as a starting point.
      Set the read marker at the end of the file
      so that the reader looks for "dead_sector" markers
      and skips already read blocks. */

   if(!(rc->readerImage = LargeOpen(Closure->imageName, O_RDONLY, IMG_PERMS)))
      Stop(_("Can't open %s:\n%s"),Closure->imageName,strerror(errno));
   if(!(rc->writerImage = LargeOpen(Closure->imageName, O_WRONLY, IMG_PERMS)))
      Stop(_("Can't open %s:\n%s"),Closure->imageName,strerror(errno));

   rc->rereading  = 1;
   rc->readMarker = image_size / 2048;

   /* Try reading the media and image fingerprints. */
      
   if(!LargeSeek(rc->readerImage, (gint64)(2048*FINGERPRINT_SECTOR)))
      unknown_fingerprint = TRUE;
   else
   {  struct MD5Context md5ctxt;
      int n = LargeRead(rc->readerImage, buf, 2048);
      int fp_read;

      MD5Init(&md5ctxt);
      MD5Update(&md5ctxt, buf, 2048);
      MD5Final(image_fp, &md5ctxt);

      fp_read = GetMediumFingerprint(rc->dh, medium_fp, FINGERPRINT_SECTOR);
	 
      if(n != 2048 || !fp_read || !memcmp(buf, Closure->deadSector, 2048))
	 unknown_fingerprint = TRUE;
   }

   /* If fingerprints could be read, compare them. */
      
   if(!unknown_fingerprint && memcmp(image_fp, medium_fp, 16))
   {  	  
      if(!Closure->guiMode)
	 Stop(_("Image file does not match the CD/DVD."));
      else
      {  int answer = ModalDialog(GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, NULL,
				  _("Image file already exists and does not match the CD/DVD.\n"
				    "The existing image file will be deleted."));
	   
	 if(!answer)
	 {  rc->unreportedError = FALSE;
	    SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
				 _("<span %s>Reading aborted.</span> Please select a different image file."),
				 Closure->redMarkup); 
	       cleanup((gpointer)rc);
	 }
	 else  /* Start over with new file */
	 {  LargeClose(rc->readerImage);
	    LargeClose(rc->writerImage);
	    LargeUnlink(Closure->imageName);
	    goto reopen_image;
	 } 
      }
   }

   /*** If the image is not complete yet, first aim to read the
	unvisited sectors before trying to re-read the missing ones. */
      
   Closure->checkCrc = 0; /* makes only sense if image is completely read */

   if(!Closure->readStart && !Closure->readEnd && rc->readMarker < rc->sectors-1)
   {  PrintLog(_("Completing image %s. Continuing with sector %lld.\n"),
	       Closure->imageName, rc->readMarker);
      rc->firstSector = rc->readMarker;
      Closure->additionalSpiralColor = 0;  /* blue */
   }
   else 
   {  PrintLog(_("Completing image %s. Only missing sectors will be read.\n"), Closure->imageName);
      Closure->additionalSpiralColor = 3;  /* dark green*/
   }
      
   if(Closure->guiMode)
      SetLabelText(GTK_LABEL(Closure->readLinearHeadline),
		   "<big>%s</big>\n<i>%s</i>",t,rc->dh->mediumDescr);

   if(Closure->guiMode)
      InitializeCurve(rc->dh->maxRate, rc->firstSector, rc->lastSector, rc->sectors);
}

/*
 * Fill the gap between rc->readMarker and rc->firstSector
 * with dead sector markers.
 */

static void fill_gap(read_closure *rc)
{  gint64 s;


   if(!rc->scanMode && rc->firstSector > rc->readMarker)
   {  s = rc->readMarker;

      if(!LargeSeek(rc->writerImage, (gint64)(2048*s)))
	Stop(_("Failed seeking to sector %lld in image [%s]: %s"),
	     s, "fill", strerror(errno));

      while(s++ < rc->firstSector)
      {  int n = LargeWrite(rc->writerImage, Closure->deadSector, 2048);
	 if(n != 2048)
	   Stop(_("Failed writing to sector %lld in image [%s]: %s"),
		s, "fill", strerror(errno));
      }
   }
}

/***
 *** Try reading the medium and create the image and map.
 ***/

/* 
 * The writer / checksum part
 */

static gpointer worker_thread(read_closure *rc)
{  gint64 s;
   int nsectors;
   int i;

   for(;;)
   {  
      /* See if more buffers are available for processing */

      g_mutex_lock(rc->mutex);

      while(rc->bufState[rc->writePtr] == BUF_EMPTY)
      {  g_cond_wait(rc->canWrite, rc->mutex);
      }

      if(rc->bufState[rc->writePtr] == BUF_EOF)
      {  g_mutex_unlock(rc->mutex);
	 return 0;
      }

      s = rc->bufferedSector[rc->writePtr];
      nsectors = rc->nSectors[rc->writePtr];
      g_mutex_unlock(rc->mutex);

      /* Write out buffer, update checksums if not in scan mode */
	
      if(!rc->scanMode)
      {  int n;

	 if(!LargeSeek(rc->writerImage, (gint64)(2048*s)))
	 {  rc->workerError = g_strdup_printf(_("Failed seeking to sector %lld in image [%s]: %s"),
					      s, "store", strerror(errno));
	    goto update_mutex;
	 }

	 n = LargeWrite(rc->writerImage, rc->alignedBuf[rc->writePtr]->buf, 2048*nsectors);
	 if(n != 2048*nsectors)
	 {  rc->workerError = g_strdup_printf(_("Failed writing to sector %lld in image [%s]: %s"),
	                                      s, "store", strerror(errno));
	    goto update_mutex;
	 }

	 /* On-the-fly CRC/MD5 calculation */

	 if(!Closure->checkCrc && Closure->crcCache)  
	 {  for(i=0; i<nsectors; i++)
	     Closure->crcCache[s+i] = Crc32(rc->alignedBuf[rc->writePtr]->buf+2048*i, 2048);

	    MD5Update(&rc->md5ctxt, rc->alignedBuf[rc->writePtr]->buf, 2048*nsectors);
	 }
      }

      /* Just do on-the-fly CRC testing if in scan mode */         

      if(Closure->checkCrc && rc->bufState[rc->writePtr] != BUF_DEAD)
	for(i=0; i<nsectors; i++)
	{  guint32 crc = Crc32(rc->alignedBuf[rc->writePtr]->buf+2048*i, 2048);

	   if(s+i < rc->ei->sectors)
	   {  if(Closure->crcCache[s+i] != crc)
	      {  PrintCLI(_("* CRC error, sector: %lld\n"), (long long int)s+i);
		 Closure->crcErrors++;
	      }
	   }
	   else Closure->crcCache[s+i] = crc; /* add CRCsums for additional sectors */

	   MD5Update(&rc->md5ctxt, rc->alignedBuf[rc->writePtr]->buf+2048*i, 2048);
	}

      if(rc->bufState[rc->writePtr] == BUF_DEAD)
	 rc->deadWritten++;

      /* Release this buffer */

update_mutex:
      g_mutex_lock(rc->mutex);
      rc->bufState[rc->writePtr] = BUF_EMPTY;
      rc->writePtr++;
      if(rc->writePtr >= READ_BUFFERS)
	rc->writePtr = 0;
      g_cond_signal(rc->canRead);
      g_mutex_unlock(rc->mutex);

      if(rc->workerError)
	return NULL;
   }

   return NULL;
}

/***
 *** The reader part
 ***/

static void insert_buttons(GtkDialog *dialog)
{  
  gtk_dialog_add_buttons(dialog, 
			 _utf("Ignore once"), 1,
			 _utf("Ignore always"), 2,
			 _utf("Abort"), 0, NULL);
} 

void ReadMediumLinear(gpointer data)
{  read_closure *rc = g_malloc0(sizeof(read_closure));
   GError *err = NULL;
   int nsectors; 
   gint64 s;
   int percent, last_percent;
   double last_speed = -1.0;
   int first_speed_value = TRUE;
   gint previous_read_errors = 0;
   gint previous_crc_errors = 0;
   char *t;
   int status,n;
   gint64 last_read_ok,last_errors_printed;
   double speed = 0.0;
   int tao_tail = 0;
   int i;

   Closure->additionalSpiralColor = -1;

   /*** Register the cleanup procedure so that Stop() can abort us properly. */

   rc->unreportedError  = TRUE;
   rc->earlyTermination = TRUE;
   rc->scanMode = GPOINTER_TO_INT(data);

   /* Register with different labels depending on rc->scanMode */

   register_reader(rc);

   /* If ecc file exists and automatic ecc creation is enabled,
      ask user if we may remove the existing one. */

   confirm_ecc_file_deletion(rc);

   /*** Timer setup */

   rc->speedTimer = g_timer_new();
   rc->readTimer  = g_timer_new();

   /*** Create the aligned buffers. */

   for(i=0; i<READ_BUFFERS; i++)
     rc->alignedBuf[i] = CreateAlignedBuffer(32768);

   /*** Open Device and query medium properties */

   rc->dh = OpenAndQueryDevice(Closure->device);
   rc->sectors = rc->dh->sectors;
   Closure->readErrors = Closure->crcErrors = rc->readOK = 0;

   /*** See if we have an ecc file which belongs to the medium */

   check_ecc_file(rc);

   /*** See if user wants to limit the read range. */

   GetReadingRange(rc->sectors, &rc->firstSector, &rc->lastSector);

   /*** Determine the reading mode. There are three possibilities:
	1. scanning (rc->scanMode == TRUE)
	2. reading into a new image file.
	3. completing an existing image file.
	After this function, files are prepared 
	and respective UI messages have been output. */

   determine_mode(rc);

   /*** If rc->firstSector > read_marker, fill the gap with dead sector markers. */

   fill_gap(rc);

   /*** Memory for the CRC32 sums is needed in two cases: */

   /* a) a full image read is attempted, and the image CRC32 
         and md5sum are calculated on the fly. */

   if(   !rc->scanMode && !rc->rereading 
      && rc->firstSector == 0 && rc->lastSector == rc->sectors-1)
   {  Closure->crcCache = g_try_malloc(sizeof(guint32) * rc->sectors);

      if(Closure->crcCache)
	Closure->crcImageName = g_strdup(Closure->imageName);

      MD5Init(&rc->md5ctxt);
   }

   /* b) we have a suitable ecc file and compare CRC32sum against
         it while reading */

   if(Closure->checkCrc)
   {  gint64 crc_sectors = rc->ei->sectors;

      if(rc->sectors > crc_sectors)  /* Allows completion of crc info */
	crc_sectors = rc->sectors;   /* when image contains additional sectors */

      Closure->crcCache = g_try_malloc(sizeof(guint32) * crc_sectors);

      if(!Closure->crcCache)
      {  Closure->checkCrc = FALSE;
      }
      else
      {  guint32 *cache = Closure->crcCache;
         int i=0;

	 if(!LargeSeek(rc->ei->file, (gint64)sizeof(EccHeader)))
	   Stop(_("Failed skipping the ecc header: %s"),strerror(errno));

	 PrintCLI("%s ...",_("Reading CRC information from ecc file"));

	 while(i<crc_sectors)
	 {  int n = i+512<crc_sectors ? 512 : crc_sectors - i;

	    if(LargeRead(rc->ei->file, cache, 4*n) != 4*n)
	      Stop(_("Error reading CRC information: %s"), strerror(errno));

	    cache += n;
	    i+=n;
	 }

         PrintCLI(_("done.\n"));

	 if(Closure->guiMode)
	   SetLabelText(GTK_LABEL(Closure->readLinearHeadline),
			"<big>%s</big>\n<i>%s</i>", rc->msg, rc->dh->mediumDescr);

	 MD5Init(&rc->md5ctxt);
      }
   }

   /*** Start the worker thread. We concentrate on reading from the drive here;
	writing the image file and calculating the checksums is done in a
	concurrent thread. */

   rc->mutex = g_mutex_new();
   rc->canRead = g_cond_new();
   rc->canWrite = g_cond_new();
   rc->worker = g_thread_create((GThreadFunc)worker_thread, (gpointer)rc, TRUE, &err);
   if(!rc->worker)
     Stop("Could not create worker thread: %s", err->message);

   /*** Prepare the speed timing */

   if(Closure->guiMode && Closure->spinupDelay)
     SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline,
			  _("Waiting %d seconds for drive to spin up...\n"), Closure->spinupDelay);

   SpinupDevice(rc->dh);

   if(Closure->guiMode && Closure->spinupDelay)
     SwitchAndSetFootline(Closure->readLinearNotebook, 0, Closure->readLinearFootline, "ignore");

   if(Closure->spinupDelay)  /* eliminate initial seek time from timing */
     ReadSectors(rc->dh, rc->alignedBuf[0]->buf, rc->firstSector, 1); 
   g_timer_start(rc->speedTimer);
   g_timer_start(rc->readTimer);

   /*** Reset for the next reading pass */

next_reading_pass:
   if(rc->pass > 0)
   {  Closure->readErrors = Closure->crcErrors = 0;
   }

   /*** Read the medium image. */

   s = rc->firstSector;
   last_percent = (1000*s)/rc->sectors;
   last_read_ok = last_errors_printed = 0;

   while(s<=rc->lastSector)
   {  if(Closure->stopActions)   /* somebody hit the Stop button */
      {
	SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
			     _("<span %s>Aborted by user request!</span> %lld sectors read, %lld sectors unreadable/skipped so far."),
			     Closure->redMarkup, rc->readOK,Closure->readErrors); 
	rc->unreportedError = FALSE;  /* suppress respective error message */
        goto terminate;
      }

      /*** Decide between reading in fast mode (16 sectors at once)
	   or reading one sector at a time.
	   Fast mode gains some reading speed due to transfering fewer
	   but larger data blocks from the device.
	   Switching to fast mode is done only on 32K boundaries
	   -- this matches the internal DVD structure better. 
           In order to treat the 2 read errors at the end of TAO discs correctly,
           we switch back to per sector reading at the end of the medium. */

      if(s & 15 || s >= ((rc->sectors - 2) & ~15) ) /* - 16 removed */ 
            nsectors = 1;
      else  nsectors = 16;

      if(s+nsectors > rc->lastSector)  /* don't read past the (CD) media end */
	nsectors = rc->lastSector-s+1;

      /*** If s is lower than the read marker,
	   check if the sector has already been read
	   in a previous session. */

reread:
      if(!rc->scanMode && s < rc->readMarker)
      {  int i,ok = 0;
	 int num_compare = nsectors;

	 if(!LargeSeek(rc->readerImage, (gint64)(2048*s)))
	   Stop(_("Failed seeking to sector %lld in image [%s]: %s"),
		s, "reread", strerror(errno));

	 if(s+nsectors > rc->readMarker)
	   num_compare = rc->readMarker-s;

	 for(i=0; i<num_compare; i++)
	 {  unsigned char sector_buf[2048];

	    n = LargeRead(rc->readerImage, sector_buf, 2048);

	    if(n != 2048)
	      Stop(_("unexpected read error in image for sector %lld"),s);

	    if(memcmp(sector_buf, Closure->deadSector, n))
	      ok++;
	 }

	 if(ok == nsectors)  /* All sectors already present. */
	 {
	   goto step_counter;
	 }
	 else                /* Some sectors still missing */
	 {
           if(nsectors > 1 && ok > 0)
	   {  nsectors = 1;
	      goto reread;
	   }
	 }
      }

     /*** Try reading the next <nsectors> sector(s). */

      g_mutex_lock(rc->mutex);
      if(rc->workerError)       /* something went wrong in the worker thread */
      {	g_mutex_unlock(rc->mutex);
	Stop(rc->workerError);
      }
      while(rc->bufState[rc->readPtr] != BUF_EMPTY)
      {  g_cond_wait(rc->canRead, rc->mutex);
      }
      g_mutex_unlock(rc->mutex);

      status = ReadSectors(rc->dh, rc->alignedBuf[rc->readPtr]->buf, s, nsectors);

      /*** Medium Error (3) and Illegal Request (5) may result from 
	   a medium read problem, but other errors are regarded as fatal. */

      if(status && !Closure->ignoreFatalSense 
	 && rc->dh->sense.sense_key && rc->dh->sense.sense_key != 3 && rc->dh->sense.sense_key != 5)
      {  int answer;

	 if(!Closure->guiMode)
	    Stop(_("Sector %lld: %s\nCan not recover from above error.\n"
		   "Use the --ignore-fatal-sense option to override."),
		 s, GetLastSenseString(FALSE));

	 answer = ModalDialog(GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, insert_buttons,
			      _("Sector %lld: %s\n\n"
				"It may not be possible to recover from this error.\n"
				"Should the reading continue and ignore this error?"),
			      s, GetLastSenseString(FALSE));

	 if(answer == 2)
	   Closure->ignoreFatalSense = 2;

	 if(!answer)
	 {  SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
				_("<span %s>Aborted by user request!</span> %lld sectors read, %lld sectors unreadable/skipped so far."),
				 Closure->redMarkup, rc->readOK,Closure->readErrors); 
	    rc->unreportedError = FALSE;  /* suppress respective error message */
	    goto terminate;
	 }
      }

      /*** Pass sector(s) to the worker thread (if reading succeeded) */

      if(!status)
      { g_mutex_lock(rc->mutex);
	rc->bufferedSector[rc->readPtr] = s;
	rc->nSectors[rc->readPtr] = nsectors;
	rc->bufState[rc->readPtr] = BUF_FULL;
	rc->readPtr++;
	if(rc->readPtr >= READ_BUFFERS)
	  rc->readPtr = 0;
	g_cond_signal(rc->canWrite);
	g_mutex_unlock(rc->mutex);

	rc->readOK += nsectors;
      }

      /*** Process the read error if reading failed. */

      if(status)
      {  int nfill;

	 /* Disable on the fly checksum calculation */

	 if(Closure->crcCache && !Closure->checkCrc)
	   ClearCrcCache();

	 /* Determine number of sectors to skip forward. 
	    Make sure not to skip past the media end
	    and to land at a multiple of 16. */

	 if(nsectors>=Closure->sectorSkip) nfill = nsectors;
	 else
	 {  if(s+Closure->sectorSkip > rc->lastSector) nfill = rc->lastSector-s+1;
	    else nfill = Closure->sectorSkip - ((s + Closure->sectorSkip) & 15);
	 }

	 /* If we are reading past the dead marker we must take care 
            to fill up any holes with dead sector markers before skipping forward. 
	    When sectorSkip is 0 and nsectors > 16, we will re-read all these sectors
	    again one by one, so we catch this case in order not to write the markers twice.  */

	 if(!rc->scanMode && s+nfill > rc->readMarker
	    && (Closure->sectorSkip || nsectors == 1))
	 {  int i;

	    /* Write nfill of "dead sector" markers so that
	       they are tried again in the following iterations / sessions. */

	    for(i=0; i<nfill; i++)
	    {
	       g_mutex_lock(rc->mutex);
	       if(rc->workerError)       /* something went wrong in the worker thread */
	       {  g_mutex_unlock(rc->mutex);
		  Stop(rc->workerError);
	       }
	       
	       while(rc->bufState[rc->readPtr] != BUF_EMPTY)
	       {  g_cond_wait(rc->canRead, rc->mutex);
	       }

	       memcpy(rc->alignedBuf[rc->readPtr]->buf, Closure->deadSector, 2048);

	       rc->bufferedSector[rc->readPtr] = s+i;
	       rc->nSectors[rc->readPtr] = 1;
	       rc->bufState[rc->readPtr] = BUF_DEAD;
	       rc->readPtr++;
	       if(rc->readPtr >= READ_BUFFERS)
		 rc->readPtr = 0;
	       g_cond_signal(rc->canWrite);
	       g_mutex_unlock(rc->mutex);
	    }
	 }

	 /* If sectorSkip is set, perform the skip.
	    nfill has been calculated so that the skip lands
	    at a multiple of 16. Therefore nsectors can remain
	    at its former value as skipping forward will not 
	    destroy 16 sector aligned reads. 
	    The nsectors>1 case can only happen when processing the tao_tail. */

	 if(Closure->sectorSkip && nsectors > 1)
	 {  PrintCLIorLabel(Closure->status,
			    _("Sector %lld: %s Skipping %d sectors.\n"),
			    s, GetLastSenseString(FALSE), nfill-1);  
	    Closure->readErrors+=nfill; 
	    s+=nfill-nsectors;   /* nsectors will be added again after the goto */
	    goto step_counter;
	 }

	 /* However, if no sector skipping is requested
	    and we are currently in fast read mode (nsectors > 1),
	    slow down to reading 1 sectors at once
	    and try to re-read the first sector. */

	 else 
	 {
	    if(nsectors > 1)
	    {  nsectors = 1;
	       goto reread;
	    }
	    else 
	    {  PrintCLIorLabel(Closure->status,
			       _("Sector %lld: %s\n"),
			       s, GetLastSenseString(FALSE));  
	       if(s >= rc->sectors - 2) tao_tail++;
	       Closure->readErrors++;
	    }
	 }
      }

      /*** Step the progress counter */

step_counter:
      if(Closure->guiMode && last_errors_printed != Closure->readErrors)
      {  SetLabelText(GTK_LABEL(Closure->readLinearErrors), 
		      _("Unreadable / skipped sectors: %lld"), Closure->readErrors);
	 last_errors_printed = Closure->readErrors;
      }

      s += nsectors;
      if(s>rc->readMarker) rc->readMarker=s;
      percent = (1000*s)/rc->sectors;

      if(last_percent != percent) 
      {  gulong ignore;
	 int color;

	 if(rc->readOK <= last_read_ok)  /* anything read since last sample? */
	 {  speed = 0.0;                 /* nothing read */
	    if(Closure->readErrors-previous_read_errors > 0)
	         color = 2;
	    else if(Closure->crcErrors-previous_crc_errors > 0)
	         color = 4;
	    else color = Closure->additionalSpiralColor;

	    if(Closure->guiMode)
	       AddCurveValues(percent, speed, color);
	    last_percent    = percent;
	    last_speed      = speed;
	    previous_read_errors = Closure->readErrors;
	    previous_crc_errors  = Closure->crcErrors;
	    last_read_ok    = rc->readOK;
	 }
	 else
	 {  double kb_read = (rc->readOK - last_read_ok) * 2.0;
	    double elapsed = g_timer_elapsed(rc->speedTimer, &ignore);
	    double kb_sec  = kb_read / elapsed;

	    if(Closure->readErrors-previous_read_errors > 0)
	         color = 2;
	    else if(Closure->crcErrors-previous_crc_errors > 0)
	         color = 4;
	    else color = 1;

	    if(first_speed_value)
	    {   speed = kb_sec / rc->dh->singleRate;

	        if(Closure->guiMode)
		{  AddCurveValues(last_percent, speed, color);
		   AddCurveValues(percent, speed, color);
		}

		first_speed_value = FALSE;
		last_percent      = percent;
		last_speed        = speed;
		previous_read_errors = Closure->readErrors;
		previous_crc_errors  = Closure->crcErrors;
		last_read_ok      = rc->readOK;
	    }
	    else
	    {  speed = (speed + kb_sec / rc->dh->singleRate) / 2.0;
	       if(speed>99.9) speed=99.9;

	       if(Closure->guiMode)
		 AddCurveValues(percent, speed, color);
	       if(Closure->speedWarning && last_speed > 0.5)
	       {  double delta = speed - last_speed;
		  double abs_delta = fabs(delta);
		  double sp = (100.0*abs_delta) / last_speed; 

		  if(sp >= Closure->speedWarning)
		  {  if(delta > 0.0)
		       PrintCLI(_("Sector %lld: Speed increased to %4.1fx\n"), s, fabs(speed));
		     else
		       PrintCLI(_("Sector %lld: Speed dropped to %4.1fx\n"), s, fabs(speed));
		  }
	       }

	       PrintProgress(_("Read position: %3d.%1d%% (%4.1fx)"),
			     percent/10,percent%10,speed);

	       last_percent    = percent;
	       last_speed      = speed;
	       previous_read_errors = Closure->readErrors;
	       previous_crc_errors = Closure->crcErrors;
	       last_read_ok    = rc->readOK;
	       g_timer_start(rc->speedTimer);
	    }
	 }
      }
   }

   /*** Consistency check */

   if(rc->deadWritten != Closure->readErrors)
      printf("Mismatch: %lld read errors, but %lld dead sectors written\n",
	     (long long int)Closure->readErrors, 
	     (long long int)rc->deadWritten);

   /*** If multiple reading passes are allowed, see if we need another pass */
#if 0
   rc->pass++;
   if((Closure->readErrors || Closure->crcErrors) && rc->pass < Closure->readMedium)
   {  t = g_strdup_printf(_("Reading pass %d of %d: %lld sectors read; %lld CRC errors; %lld missing."),
			  rc->pass, Closure->readMedium, 
			  rc->readOK, Closure->crcErrors, Closure->readErrors);
      PrintLog("\n%s\n",t);
      goto next_reading_pass;
   }
#endif

   /*** Signal EOF to writer thread; wait for it to finish */

   send_eof(rc);
   g_thread_join(rc->worker);
   rc->worker = NULL;

   /*** Print summary */

   if(Closure->crcCache)
     MD5Final(Closure->md5Cache, &rc->md5ctxt);

   if(rc->rereading)
   {  if(!Closure->readErrors) t = g_strdup_printf(_("%lld sectors read.     "),rc->readOK);
      else                t = g_strdup_printf(_("%lld sectors read; %lld unreadable sectors."),rc->readOK,Closure->readErrors);
   }
   else
   {  if(!Closure->readErrors && !Closure->crcErrors) 
      {  
	 if(Closure->checkCrc && rc->ei)
	 {  
	    if(rc->dh->sectors != rc->ei->sectors)
	      t = g_strdup_printf(_("All sectors successfully read, but wrong image length (%lld sectors difference)"), rc->dh->sectors - rc->ei->sectors);
	    else if(   rc->readOK == rc->sectors  /* no user limited range */  
		    && memcmp(rc->ei->eh->mediumSum, Closure->md5Cache, 16))
	      t = g_strdup_printf(_("All sectors successfully read, but wrong image checksum."));
	    else t = g_strdup_printf(_("All sectors successfully read. Checksums match."));
	 }
	 else t = g_strdup_printf(_("All sectors successfully read."));
      }
      else 
      {  if(Closure->readErrors && !Closure->crcErrors)
	      t = g_strdup_printf(_("%lld unreadable sectors."),Closure->readErrors);
         else if(!Closure->readErrors && Closure->crcErrors)
	      t = g_strdup_printf(_("%lld CRC errors."),Closure->crcErrors);
	 else t = g_strdup_printf(_("%lld CRC errors, %lld unreadable sectors."),Closure->crcErrors, Closure->readErrors);
      }
   }
   PrintLog("\n%s\n",t);
   if(Closure->guiMode)
   {  if(rc->scanMode) SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
					 "%s%s",_("Scanning finished: "),t);
      else             SwitchAndSetFootline(Closure->readLinearNotebook, 1, Closure->readLinearFootline, 
					 "%s%s",_("Reading finished: "),t);
   }
   g_free(t);

   PrintTimeToLog(rc->readTimer, "for reading/scanning.\n");

   if(rc->dh->subType != DVD && tao_tail && tao_tail == Closure->readErrors && !Closure->noTruncate)
   {  int answer;
   
      if(Closure->guiMode)
        answer = ModalWarning(GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL, NULL,
			      _("%d sectors missing at the end of the disc.\n"
				"This is okay if the CD was written in TAO (track at once) mode.\n"
				"The Image will be truncated accordingly. See the manual for details.\n"),
			      tao_tail);
      else 
        answer = ModalWarning(GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL, NULL,
			      _("%d sectors missing at the end of the disc.\n"
				"This is okay if the CD was written in TAO (track at once) mode.\n"
				"The Image will be truncated accordingly. See the manual for details.\n"
				"Use the --dao option to disable image truncating.\n"),
			      tao_tail);
     
      rc->sectors -= tao_tail;

      if(!rc->scanMode && answer)
        if(!LargeTruncate(rc->writerImage, (gint64)(2048*rc->sectors)))
	  Stop(_("Could not truncate %s: %s\n"),Closure->imageName,strerror(errno));
   }
   else if(Closure->readErrors) exitCode = EXIT_FAILURE;

   /*** Eject medium */

   if(Closure->eject && !Closure->readErrors)
      LoadMedium(rc->dh, FALSE);

   /*** Close and clean up */

   rc->unreportedError = FALSE;
   rc->earlyTermination = FALSE;

terminate:
   cleanup((gpointer)rc);
}

