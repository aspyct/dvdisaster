/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2006 Carsten Gnoerlich.
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

#include "rs02-includes.h"

/***
 *** Reset the compare output window
 ***/

void ResetRS02CompareWindow(Method *self)
{  RS02Widgets *wl = (RS02Widgets*)self->widgetList;

   SetLabelText(GTK_LABEL(wl->cmpImageSectors), "");
   SetLabelText(GTK_LABEL(wl->cmpImageMd5Sum), "");
   SetLabelText(GTK_LABEL(wl->cmpEccHeaders), "");
   SetLabelText(GTK_LABEL(wl->cmpDataSection), "");
   SetLabelText(GTK_LABEL(wl->cmpCrcSection), "");
   SetLabelText(GTK_LABEL(wl->cmpEccSection), "");
   SetLabelText(GTK_LABEL(wl->cmpImageResult), "");

   SetLabelText(GTK_LABEL(wl->cmpEccCreatedBy), "dvdisaster");
   SetLabelText(GTK_LABEL(wl->cmpEccMethod), "");
   SetLabelText(GTK_LABEL(wl->cmpEccRequires), "");
   SetLabelText(GTK_LABEL(wl->cmpEccMediumSectors), "");
   SetLabelText(GTK_LABEL(wl->cmpDataMd5Sum), "");
   SetLabelText(GTK_LABEL(wl->cmpCrcMd5Sum), "");
   SetLabelText(GTK_LABEL(wl->cmpEccMd5Sum), "");

   SwitchAndSetFootline(wl->cmpEccNotebook, 0, NULL, NULL);

   Closure->percent = 0;
   Closure->lastPercent = 0;

   FillSpiral(wl->cmpSpiral, Closure->background);
   DrawSpiral(wl->cmpSpiral);
}

/***
 *** Manage the image spiral
 ***/

/*
 * Update part of the spiral
 */

typedef struct _spiral_idle_info
{  Spiral *cmpSpiral;
   GdkColor *segColor;
} spiral_idle_info;

static gboolean spiral_idle_func(gpointer data)
{  spiral_idle_info *sii = (spiral_idle_info*)data;
   int i;

   for(i=Closure->lastPercent+1; i<=Closure->percent; i++)
     DrawSpiralSegment(sii->cmpSpiral, sii->segColor, i-1);

   Closure->lastPercent = Closure->percent;

   g_free(sii);
   return FALSE;
}

static void add_compare_values(Method *method, int percent, 
/*			       gint64 totalMissing, gint64 totalCrcErrors, */
			       gint64 newMissing, gint64 newCrcErrors)
{  RS02Widgets *wl = (RS02Widgets*)method->widgetList;
   spiral_idle_info *sii = g_malloc(sizeof(spiral_idle_info));

   if(percent < 0 || percent > COMPARE_IMAGE_SEGMENTS)
     return;

   /*
   if(newMissing) 
     SetLabelText(GTK_LABEL(wl->cmpMissingSectors), "<span color=\"red\">%lld</span>", totalMissing);

   if(newCrcErrors) 
     SetLabelText(GTK_LABEL(wl->cmpChkSumErrors), "<span color=\"red\">%lld</span>", totalCrcErrors);
   */

   sii->cmpSpiral = wl->cmpSpiral;

   sii->segColor = Closure->green;
   if(newCrcErrors) sii->segColor = Closure->yellow;
   if(newMissing) sii->segColor = Closure->red;

   Closure->percent = percent;

   g_idle_add(spiral_idle_func, sii);
}

/*
 * Redraw whole spiral
 */

static void redraw_spiral(RS02Widgets *wl)
{  int x = wl->cmpSpiral->mx - wl->cmpSpiral->diameter/2 + 10;

   DrawSpiralLabel(wl->cmpSpiral, wl->cmpLayout,
		   _("Good sectors"), Closure->green, x, 1);

   DrawSpiralLabel(wl->cmpSpiral, wl->cmpLayout,
		   _("Sectors with CRC errors"), Closure->yellow, x, 2);

   DrawSpiralLabel(wl->cmpSpiral, wl->cmpLayout,
		   _("Missing sectors"), Closure->red, x, 3);

   DrawSpiral(wl->cmpSpiral);
}

/*
 * expose event handler for the spiral
 */

static gboolean expose_cb(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{  RS02Widgets *wl = (RS02Widgets*)data;
   GtkAllocation *a = &widget->allocation;
   int w,h,size;

   /* Finish spiral initialization */

   if(!wl->cmpLayout)
   {  SetSpiralWidget(wl->cmpSpiral, widget);
      wl->cmpLayout = gtk_widget_create_pango_layout(widget, NULL); REMEMBER(wl->cmpLayout);
   }

   SetText(wl->cmpLayout, _("Missing sectors"), &w, &h);
   size = wl->cmpSpiral->diameter + 20 + 3*(10+h);  /* approx. size of spiral + labels */

   wl->cmpSpiral->mx = a->width / 2;
   wl->cmpSpiral->my = (wl->cmpSpiral->diameter + a->height - size)/2;

   if(!event->count)      /* Exposure compression */
     redraw_spiral(wl);   /* Redraw the spiral */

   return TRUE;
}

/***
 *** Create the notebook contents for the compare output
 ***/

void CreateRS02CompareWindow(Method *self, GtkWidget *parent)
{  RS02Widgets *wl = (RS02Widgets*)self->widgetList;
   GtkWidget *sep,*notebook,*table,*table2,*ignore,*lab,*frame,*d_area;

   wl->cmpHeadline = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(wl->cmpHeadline), 0.0, 0.0); 
   gtk_misc_set_padding(GTK_MISC(wl->cmpHeadline), 5, 0);
   gtk_box_pack_start(GTK_BOX(parent), wl->cmpHeadline, FALSE, FALSE, 3);

   sep = gtk_hseparator_new();
   gtk_box_pack_start(GTK_BOX(parent), sep, FALSE, FALSE, 0);

   sep = gtk_hseparator_new();
   gtk_box_pack_start(GTK_BOX(parent), sep, FALSE, FALSE, 0);

   table = gtk_table_new(2, 2, FALSE);
   gtk_container_set_border_width(GTK_CONTAINER(table), 5);
   gtk_box_pack_start(GTK_BOX(parent), table, TRUE, TRUE, 0);

   /*** Image info */

   frame = gtk_frame_new(_utf("Image file summary"));
   gtk_table_attach(GTK_TABLE(table), frame, 0, 1, 0, 1, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 5);

   table2 = gtk_table_new(2, 7, FALSE);
   ignore = gtk_label_new("image info");
   gtk_container_set_border_width(GTK_CONTAINER(table2), 5);
   gtk_container_add(GTK_CONTAINER(frame), table2);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Medium sectors:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 0, 1, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpImageSectors = gtk_label_new("0");
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Data checksum:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 1, 2, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpImageMd5Sum = gtk_label_new("0");
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Ecc headers:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 2, 3, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccHeaders = gtk_label_new(".");
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 2, 3, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Data section:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 3, 4, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpDataSection = gtk_label_new(".");
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 3, 4, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Crc section:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 4, 5, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpCrcSection = gtk_label_new(".");
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 4, 5, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Ecc section:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 5, 6, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccSection= gtk_label_new(".");
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 5, 6, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = wl->cmpImageResult = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 2, 6, 7, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 4);

   /*** Image spiral */

   frame = gtk_frame_new(_utf("Image state"));
   gtk_table_attach(GTK_TABLE(table), frame, 1, 2, 0, 2, GTK_SHRINK | GTK_FILL, GTK_EXPAND | GTK_FILL, 5, 5);

   wl->cmpSpiral = CreateSpiral(Closure->grid, Closure->background, 10, 5, COMPARE_IMAGE_SEGMENTS-1);
   d_area = wl->cmpDrawingArea = gtk_drawing_area_new();
   gtk_widget_set_size_request(d_area, wl->cmpSpiral->diameter+20, -1);
   gtk_container_add(GTK_CONTAINER(frame), d_area);
   g_signal_connect(G_OBJECT(d_area), "expose_event", G_CALLBACK(expose_cb), (gpointer)wl);

   /*** Ecc data info */

   frame = gtk_frame_new(_utf("Error correction data"));
   gtk_table_attach(GTK_TABLE(table), frame, 0, 1, 1, 2, GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 5, 5);

   notebook = wl->cmpEccNotebook = gtk_notebook_new();
   gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
   gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
   gtk_container_add(GTK_CONTAINER(frame), notebook);

   ignore = gtk_label_new(NULL);
   lab = gtk_label_new("");
   gtk_notebook_append_page(GTK_NOTEBOOK(notebook), lab, ignore);

   table2 = gtk_table_new(2, 8, FALSE);
   ignore = gtk_label_new("ecc info");
   gtk_notebook_append_page(GTK_NOTEBOOK(notebook), table2, ignore);
   gtk_container_set_border_width(GTK_CONTAINER(table2), 5);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Created by:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 0, 1, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccCreatedBy = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Method:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 1, 2, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccMethod = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Requires:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 2, 3, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccRequires = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 2, 3, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Medium sectors:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 3, 4, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccMediumSectors = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 3, 4, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Data checksum:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 4, 5, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpDataMd5Sum = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 4, 5, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("CRC checksum:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 5, 6, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpCrcMd5Sum = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 5, 6, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);
   lab = gtk_label_new(NULL);

   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   SetLabelText(GTK_LABEL(lab), _("Ecc checksum:"));
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 1, 6, 7, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 2 );
   lab = wl->cmpEccMd5Sum = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0); 
   gtk_table_attach(GTK_TABLE(table2), lab, 1, 2, 6, 7, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);

   lab = wl->cmpEccResult = gtk_label_new(NULL);
   gtk_misc_set_alignment(GTK_MISC(lab), 0.0, 0.0);
   gtk_table_attach(GTK_TABLE(table2), lab, 0, 2, 7, 8, GTK_SHRINK | GTK_FILL, GTK_SHRINK, 5, 4);

}

/***
 *** Check the consistency of the augmented image
 ***/

/* 
 * housekeeping
 */

typedef struct
{  LargeFile *file;
   RS02Layout *lay;
   guint32 *crcBuf;
   gint8   *crcValid;
   unsigned char crcSum[16];
} compare_closure;

static void cleanup(gpointer data)
{  compare_closure *cc = (compare_closure*)data;

   Closure->cleanupProc = NULL;

   if(Closure->guiMode)
      AllowActions(TRUE);

   if(cc->file) LargeClose(cc->file);
   if(cc->lay) g_free(cc->lay);
   if(cc->crcBuf) g_free(cc->crcBuf);
   if(cc->crcValid) g_free(cc->crcValid);
   
   g_free(cc);

   g_thread_exit(0);
}

/***
 *** Read the crc portion and descramble it from ecc block order
 *** into ascending sector order. 
 */

static void read_crc(compare_closure *cc, RS02Layout *lay)
{  struct MD5Context crc_md5;
   gint64 block_idx[256];
   guint32 crc_buf[512];
   gint64 s;
   int i,crc_idx;
   int crc_valid = 1;

   /* Allocate buffer for ascending sector order CRCs */

   cc->crcBuf   = g_malloc(2048 * lay->crcSectors);
   cc->crcValid = g_malloc(512 * lay->crcSectors);
   MD5Init(&crc_md5);

   /* First sector containing crc data */

   if(!LargeSeek(cc->file, 2048*(lay->dataSectors+2)))
     Stop(_("Failed seeking to sector %lld in image: %s"), 
	  lay->dataSectors+2, strerror(errno));

   /* Initialize ecc block index pointers.
      The first CRC set (of lay->ndata checksums) relates to
      ecc block lay->firstCrcLayerIndex + 1. */

   for(s=0, i=0; i<lay->ndata; s+=lay->sectorsPerLayer, i++)
     block_idx[i] = s + lay->firstCrcLayerIndex + 1;

   crc_idx = 512;  /* force crc buffer reload */

   /* Cycle through the ecc blocks and sort CRC sums in
      ascending sector numbers. */

   for(s=0; s<lay->sectorsPerLayer; s++)
   {  gint64 si = (s + lay->firstCrcLayerIndex + 1) % lay->sectorsPerLayer;

      /* Wrap the block_idx[] ptrs at si == 0 */

      if(!si)
      {  gint64 bs;

         for(bs=0, i=0; i<lay->ndata; bs+=lay->sectorsPerLayer, i++)
	   block_idx[i] = bs;
      }

      /* Go through all data sectors of current ecc block */

      for(i=0; i<lay->ndata; i++)
      {
	 if(block_idx[i] < lay->dataSectors)  /* only data sectors have CRCs */
	 {  
	    /* Refill crc cache if needed */
	    
	    if(crc_idx >= 512)
	    {   if(LargeRead(cc->file, crc_buf, 2048) != 2048)
		  Stop(_("problem reading crc data: %s"), strerror(errno));

	        crc_valid = memcmp(crc_buf, Closure->deadSector, 2048);
		
	        MD5Update(&crc_md5, (unsigned char*)crc_buf, 2048);
		crc_idx = 0;
	    }

	    /* Sort crc into appropriate place */

	    cc->crcBuf[block_idx[i]]   = crc_buf[crc_idx];
       	    cc->crcValid[block_idx[i]] = crc_valid;
	    crc_idx++;
	    block_idx[i]++;
	 }
      }
   }

   MD5Final(cc->crcSum, &crc_md5);
}

/*
 * The compare action
 */

void RS02Compare(Method *self)
{  compare_closure *cc = g_malloc0(sizeof(compare_closure));
   RS02Widgets *wl = self->widgetList;
   LargeFile *image;
   EccHeader *eh;
   RS02Layout *lay;
   struct MD5Context image_md5;
   struct MD5Context ecc_md5;
   struct MD5Context meta_md5;
   unsigned char ecc_sum[16];
   unsigned char medium_sum[16];
   char data_digest[33], hdr_digest[33], digest[33];
   gint64 s, image_sectors, crc_idx;
   int last_percent = 0;
   unsigned char buf[2048];
   gint64 first_missing, last_missing;
   gint64 total_missing,data_missing,crc_missing,ecc_missing,hdr_missing;
   gint64 new_missing = 0, new_crc_errors = 0;
   gint64 data_crc_errors,hdr_crc_errors;
   gint64 hdr_ok,hdr_pos;
   gint64 ecc_sector,expected_sectors;
   int ecc_slice;
   int major,minor,pl;
   char method[5];
   char *img_advice = NULL;
   char *ecc_advice = NULL;

   /*** Prepare for early termination */

   RegisterCleanup(_("Check aborted"), cleanup, cc);

   /*** Open the .iso file */

   LargeStat(Closure->imageName, &image_sectors);
   image_sectors /= 2048;
   image = cc->file = LargeOpen(Closure->imageName, O_RDONLY, IMG_PERMS);

   if(!image)  /* Failing here is unlikely since caller could open it */
     Stop("Could not open %s: %s",Closure->imageName, strerror(errno));

   if(Closure->guiMode)
     SetLabelText(GTK_LABEL(wl->cmpHeadline), "<big>%s</big>\n<i>%s</i>",
		  _("Checking the image file."),
		  _("Image contains error correction data."));

   PrintLog("\n%s: ",Closure->imageName);
   PrintLog(_("present, contains %lld medium sectors.\n"),image_sectors);

   eh  = self->lastEh;  /* will always be present */
   lay = cc->lay = CalcRS02Layout(uchar_to_gint64(eh->sectors), eh->eccBytes); 
   expected_sectors = lay->eccSectors+lay->dataSectors;
   if(!eh->inLast)      /* 0.66 pre-releases did not set this */
     eh->inLast = 2048;

   /*** Print information on image size */

   if(Closure->guiMode)
   {  if(expected_sectors == image_sectors)
      {  SetLabelText(GTK_LABEL(wl->cmpImageSectors), "%lld", image_sectors);
      }
      else
      {  SetLabelText(GTK_LABEL(wl->cmpImageSectors), "<span color=\"red\">%lld</span>", image_sectors);
	 if(expected_sectors > image_sectors)
	      img_advice = g_strdup_printf(_("<span color=\"red\">Image file is %lld sectors shorter than expected.</span>"), expected_sectors - image_sectors);
	 else img_advice = g_strdup_printf(_("<span color=\"red\">Image file is %lld sectors longer than expected.</span>"), image_sectors - expected_sectors);
      }
   }

   /*** Check integrity of the ecc headers */

   hdr_ok = hdr_missing = hdr_crc_errors = 0;
   hdr_pos = lay->firstEccHeader;

   while(hdr_pos < expected_sectors)
   {  EccHeader eh;

      if(hdr_pos < image_sectors)
      {  int n;

	 if(!LargeSeek(image, 2048*hdr_pos))
	   Stop(_("Failed seeking to ecc header at %lld: %s\n"), hdr_pos, strerror(errno));

	 n = LargeRead(image, &eh, sizeof(EccHeader));
	 if(n != sizeof(EccHeader))
	   Stop(_("Failed reading ecc header at %lld: %s\n"), hdr_pos, strerror(errno));
      }
      else memset(&eh, 0, sizeof(EccHeader));

      if(!strncmp((char*)eh.cookie, "*dvdisaster*", 12))
      {  guint32 recorded_crc = eh.selfCRC;
 	 guint32 real_crc;

 	 eh.selfCRC = 0x4c5047;
         real_crc = Crc32((unsigned char*)&eh, sizeof(EccHeader));

	 if(real_crc == recorded_crc)
	    hdr_ok++;
	 else
	 {  hdr_crc_errors++; 
	 }
      }
      else hdr_missing++;

      if(hdr_pos == lay->firstEccHeader)
	   hdr_pos = (lay->protectedSectors + lay->headerModulo - 1) & ~(lay->headerModulo-1);
      else hdr_pos += lay->headerModulo;

      if(Closure->guiMode)
      {  if(!hdr_crc_errors && !hdr_missing)
	    SetLabelText(GTK_LABEL(wl->cmpEccHeaders), _("complete"));
         else
	 {  SetLabelText(GTK_LABEL(wl->cmpEccHeaders), _("<span color=\"red\">%lld ok, %lld CRC errors, %lld missing</span>"),
			 hdr_ok, hdr_crc_errors, hdr_missing);
	 }
      }
   }

   /*** Read the CRC portion */ 

   read_crc(cc, lay);

   /*** Check the data portion of the image file for the
	"dead sector marker" and CRC errors */
   
   if(!LargeSeek(image, 0))
     Stop(_("Failed seeking to start of image: %s\n"), strerror(errno));

   MD5Init(&image_md5);
   MD5Init(&ecc_md5);
   MD5Init(&meta_md5);

   first_missing = last_missing = -1;
   total_missing = data_missing = crc_missing = ecc_missing = 0;
   data_crc_errors = 0;
   crc_idx = 0;

   ecc_sector = 0;
   ecc_slice  = 0;

   for(s=0; s<expected_sectors; s++)
   {  int percent,current_missing;

      /* Check for user interruption */

      if(Closure->stopActions)   
      {  SetLabelText(GTK_LABEL(wl->cmpImageResult), 
		      _("<span color=\"red\">Aborted by user request!</span>")); 
         goto terminate;
      }

      /* Read the next sector */

      if(s < image_sectors)  /* image may be truncated */
      {  int n = LargeRead(image, buf, 2048);
         if(n != 2048)
	    Stop(_("premature end in image (only %d bytes): %s\n"),n,strerror(errno));
      }
      else memcpy(buf, Closure->deadSector, 2048);

      if(s < lay->dataSectors)
      {  if(s < lay->dataSectors - 1)
	      MD5Update(&image_md5, buf, 2048);
	 else MD5Update(&image_md5, buf, eh->inLast);
      }

      /* Look for the dead sector marker */

      current_missing = !memcmp(buf, Closure->deadSector, 2048);
      if(current_missing)
      {  if(first_missing < 0) first_missing = s;
         last_missing = s;
	 total_missing++;
	 new_missing++;
	 if(s < lay->dataSectors) data_missing++;
	 else if(s >= lay->dataSectors + 2 && s < lay->protectedSectors) crc_missing++;
	 else ecc_missing++;
      }

      /* Report dead sectors. Combine subsequent missing sectors into one report. */

      if(!current_missing || s==expected_sectors-1)
      {  if(first_missing>=0)
	 {   if(first_missing == last_missing)
	           PrintCLI(_("* missing sector   : %lld\n"), first_missing);
	     else PrintCLI(_("* missing sectors  : %lld - %lld\n"), first_missing, last_missing);
	     first_missing = -1;
	 }
      }

      /* If the image sector is from the data portion and it was readable, 
	 test its CRC sum */

      if(s < lay->dataSectors && !current_missing)
      {  guint32 crc = Crc32(buf, 2048);

	 if(cc->crcValid[crc_idx] && crc != cc->crcBuf[crc_idx])
	 {  PrintCLI(_("* CRC error, sector: %lld\n"), s);
	    data_crc_errors++;
	    new_crc_errors++;
	 }
      }
      crc_idx++;

      /* Calculate the ecc checksum */

      if(s == RS02EccSectorIndex(lay, ecc_slice, ecc_sector))
      {  MD5Update(&ecc_md5, buf, 2048);
	 ecc_sector++;
	 if(ecc_sector >= lay->sectorsPerLayer)
	 {  MD5Final(ecc_sum, &ecc_md5); 
	    MD5Init(&ecc_md5);
	    MD5Update(&meta_md5, ecc_sum, 16);

	    ecc_sector = 0;
	    ecc_slice++;
	 }
      }

      if(Closure->guiMode) 
	    percent = (COMPARE_IMAGE_SEGMENTS*s)/expected_sectors;
      else  percent = (100*s)/expected_sectors;

      if(last_percent != percent) 
      {  PrintProgress(_("- testing sectors  : %3d%%") ,percent);
	 if(Closure->guiMode)
	 {  add_compare_values(self, percent, new_missing, new_crc_errors); 
	    if(data_missing || data_crc_errors)
	      SetLabelText(GTK_LABEL(wl->cmpDataSection), 
			   _("<span color=\"red\">%lld sectors missing; %lld CRC errors</span>"),
			   data_missing, data_crc_errors);
	    if(crc_missing)
	      SetLabelText(GTK_LABEL(wl->cmpCrcSection), 
			   _("<span color=\"red\">%lld sectors missing</span>"),
			   crc_missing);
	    if(ecc_missing)
	      SetLabelText(GTK_LABEL(wl->cmpEccSection), 
			   _("<span color=\"red\">%lld sectors missing</span>"),
			   ecc_missing);
	 }
	 last_percent = percent;
	 new_missing = new_crc_errors = 0;
      }
   }

   /* Complete damage summary */

   if(Closure->guiMode)
   {  if(data_missing || data_crc_errors)
        SetLabelText(GTK_LABEL(wl->cmpDataSection), 
		     _("<span color=\"red\">%lld sectors missing; %lld CRC errors</span>"),
		     data_missing, data_crc_errors);
      if(crc_missing)
	SetLabelText(GTK_LABEL(wl->cmpCrcSection), 
		     _("<span color=\"red\">%lld sectors missing</span>"),
		     crc_missing);
      if(ecc_missing)
	SetLabelText(GTK_LABEL(wl->cmpEccSection), 
		     _("<span color=\"red\">%lld sectors missing</span>"),
		     ecc_missing);
   }

   /* The image md5sum is only useful if all blocks have been successfully read. */

   MD5Final(medium_sum, &image_md5);
   AsciiDigest(data_digest, medium_sum);

   MD5Final(ecc_sum, &meta_md5); 
	    

   PrintProgress(_("- testing sectors  : %3d%%"), 100);

   /* Do a resume of our findings */ 

   if(!total_missing && !hdr_missing && !hdr_crc_errors && !data_crc_errors)
      PrintLog(_("- good image       : all sectors present\n"
		 "- data md5sum      : %s\n"),data_digest);
   else
   {  gint64 total_crc_errors = data_crc_errors + hdr_crc_errors;

      if(!total_missing && !total_crc_errors)
         PrintLog(_("* suspicious image : contains damaged ecc headers\n"));
      else
      {  if(!total_crc_errors)
	   PrintLog(_("* BAD image        : %lld sectors missing\n"), total_missing);
	 if(!total_missing)
	   PrintLog(_("* suspicious image : all sectors present, but %lld CRC errors\n"), total_crc_errors);
	 if(total_missing && total_crc_errors)
	   PrintLog(_("* BAD image        : %lld sectors missing, %lld CRC errors\n"), 
		    total_missing, total_crc_errors);
      }

      PrintLog(_("  ... ecc headers    : %lld ok, %lld CRC errors, %lld missing\n"),
		 hdr_ok, hdr_crc_errors, hdr_missing);
      PrintLog(_("  ... data section   : %lld sectors missing; %lld CRC errors\n"), 
	       data_missing, data_crc_errors);
      if(!data_missing)
	PrintLog(_("  ... data md5sum    : %s\n"), data_digest); 
      PrintLog(_("  ... crc section    : %lld sectors missing\n"), crc_missing);
      PrintLog(_("  ... ecc section    : %lld sectors missing\n"), ecc_missing);
   }

   if(Closure->guiMode)
   {  if(!data_missing && !data_crc_errors) 
                        SetLabelText(GTK_LABEL(wl->cmpDataSection), _("complete"));
      if(!crc_missing)  SetLabelText(GTK_LABEL(wl->cmpCrcSection), _("complete"));
      if(!ecc_missing)  SetLabelText(GTK_LABEL(wl->cmpEccSection), _("complete"));
     
      SetLabelText(GTK_LABEL(wl->cmpImageMd5Sum), "%s", data_missing ? "-" : data_digest);

      if(img_advice) 
      {  SetLabelText(GTK_LABEL(wl->cmpImageResult), img_advice);
         g_free(img_advice);
      }
      else 
      {  if(!total_missing && !hdr_missing && !hdr_crc_errors && !data_crc_errors)
	   SetLabelText(GTK_LABEL(wl->cmpImageResult),
			_("<span color=\"#008000\">Good image.</span>"));
	 else
           SetLabelText(GTK_LABEL(wl->cmpImageResult),
			_("<span color=\"red\">Damaged image.</span>"));
      }
   }

   /*** Print some information on the ecc portion */

   PrintLog(_("\nError correction data: "));

   major = eh->creatorVersion/10000; 
   minor = (eh->creatorVersion%10000)/100;
   pl    = eh->creatorVersion%100;

   if(eh->creatorVersion%100)        
   {  char *format, *color_format = NULL;

      if(eh->methodFlags[3] & MFLAG_DEVEL) 
      {  format = "%s-%d.%d (devel-%d)";
	 color_format = "%s-%d.%d <span color=\"red\">(devel-%d)</span>";
      }
      else if(eh->methodFlags[3] & MFLAG_RC) 
      {  format = "%s-%d.%d (rc-%d)";
	 color_format = "%s-%d.%d <span color=\"red\">(rc-%d)</span>";
      }
      else format = "%s-%d.%d (pl%d)";

      PrintLog(format, _("created by dvdisaster"), major, minor, pl);
      PrintLog("\n");

      if(!color_format) color_format = format;
      if(Closure->guiMode)
	SwitchAndSetFootline(wl->cmpEccNotebook, 1,
			     wl->cmpEccCreatedBy, 
			     color_format, "dvdisaster",
			     major, minor, pl);
   }
   else
   {  PrintLog(_("created by dvdisaster-%d.%d\n"), 
	       major, minor);

      if(Closure->guiMode)
	SwitchAndSetFootline(wl->cmpEccNotebook, 1,
			     wl->cmpEccCreatedBy, "dvdisaster-%d.%d",
			     major, minor);
   }

   /* Error correction method */

   memcpy(method, eh->method, 4); method[4] = 0;

   PrintLog(_("- method           : %4s, %d roots, %4.1f%% redundancy.\n"),
	    method, eh->eccBytes, 
	    ((double)eh->eccBytes*100.0)/(double)eh->dataBytes);

   if(Closure->guiMode)
     SetLabelText(GTK_LABEL(wl->cmpEccMethod), _("%4s, %d roots, %4.1f%% redundancy"),
		  method, eh->eccBytes, 
		  ((double)eh->eccBytes*100.0)/(double)eh->dataBytes);

   /* required dvdisaster version */

   if(!VerifyVersion(eh, 0))
   {  PrintLog(_("- requires         : dvdisaster-%d.%d (good)\n"),
	       eh->neededVersion/10000,
	       (eh->neededVersion%10000)/100);


      if(Closure->guiMode)
	SetLabelText(GTK_LABEL(wl->cmpEccRequires), "dvdisaster-%d.%d",
		     eh->neededVersion/10000,
		     (eh->neededVersion%10000)/100);
   }
   else 
   {  PrintLog(_("* requires         : dvdisaster-%d.%d (BAD)\n"
		 "* Warning          : The following output might be incorrect.\n"
		 "*                  : Please visit http://www.dvdisaster.com for an upgrade.\n"),
	       eh->neededVersion/10000,
	       (eh->neededVersion%10000)/100);


     if(Closure->guiMode)
     {  SetLabelText(GTK_LABEL(wl->cmpEccRequires), 
		     "<span color=\"red\">dvdisaster-%d.%d</span>",
		     eh->neededVersion/10000,
		     (eh->neededVersion%10000)/100);
        if(!ecc_advice) 
	  ecc_advice = g_strdup(_("<span color=\"red\">Please upgrade your version of dvdisaster!</span>"));
     }
   }

   /* Number of sectors medium is supposed to have */

   if(image_sectors == expected_sectors)
   {  PrintLog(_("- medium sectors   : %lld (good)\n"), expected_sectors);

      if(Closure->guiMode)
	SetLabelText(GTK_LABEL(wl->cmpEccMediumSectors), "%lld", expected_sectors);
   }
   else 
   {  if(image_sectors > expected_sectors && image_sectors - expected_sectors <= 2)   
           PrintLog(_("* medium sectors   : %lld (BAD, perhaps TAO/DAO mismatch)\n"),
		    expected_sectors);
      else PrintLog(_("* medium sectors   : %lld (BAD)\n"),expected_sectors);

      if(Closure->guiMode)
      {  SetLabelText(GTK_LABEL(wl->cmpEccMediumSectors), 
		      "<span color=\"red\">%lld</span>", expected_sectors);
	 if(!ecc_advice && image_sectors > expected_sectors)
	   ecc_advice = g_strdup(_("<span color=\"red\">Image size does not match recorded size.</span>"));
      }
   }

   /* image md5sum as stored in the ecc header */

   AsciiDigest(hdr_digest, eh->mediumSum);

   if(!data_missing)
   {  int n = !memcmp(eh->mediumSum, medium_sum, 16);

      if(n) PrintLog(_("- data md5sum      : %s (good)\n"),hdr_digest);
      else  PrintLog(_("* data md5sum      : %s (BAD)\n"),hdr_digest);

      if(Closure->guiMode)
      {  if(n) SetLabelText(GTK_LABEL(wl->cmpDataMd5Sum), "%s", hdr_digest);
	 else  
	 {  SetLabelText(GTK_LABEL(wl->cmpDataMd5Sum), "<span color=\"red\">%s</span>", hdr_digest);
	    SetLabelText(GTK_LABEL(wl->cmpImageMd5Sum), "<span color=\"red\">%s</span>", data_digest);
	 }
      }
   }
   else 
   {  PrintLog(_("- data md5sum      : %s\n"), "-");

      if(Closure->guiMode)
	SetLabelText(GTK_LABEL(wl->cmpDataMd5Sum), "%s", "-");
   }

   /*** md5sum of the crc portion */

   AsciiDigest(digest, cc->crcSum);

   if(!crc_missing)
   {  if(!memcmp(eh->crcSum, cc->crcSum, 16))
      {  PrintLog(_("- crc md5sum       : %s (good)\n"),digest);
         if(Closure->guiMode)
	   SetLabelText(GTK_LABEL(wl->cmpCrcMd5Sum), "%s", digest);
      }
      else 
      {    PrintLog(_("* crc md5sum       : %s (BAD)\n"),digest);
           if(Closure->guiMode)
	   {  SetLabelText(GTK_LABEL(wl->cmpCrcMd5Sum), "<span color=\"red\">%s</span>", digest);
	      if(!ecc_advice)
	        ecc_advice = g_strdup(_("<span color=\"red\">Damaged CRC data.</span>"));
	   }
      }
   }
   else
   {  PrintLog(_("- crc md5sum       : %s\n"), "-");
     
      if(Closure->guiMode)
	SetLabelText(GTK_LABEL(wl->cmpCrcMd5Sum), "%s", "-");
   }

   /*** meta md5sum of the ecc slices */

   AsciiDigest(digest, ecc_sum);

   if(!ecc_missing)
   {  if(!memcmp(eh->eccSum, ecc_sum, 16))
      {    PrintLog(_("- ecc md5sum       : %s (good)\n"),digest);
           if(Closure->guiMode)
	      SetLabelText(GTK_LABEL(wl->cmpEccMd5Sum), "%s", digest);
      }
      else 
      {    PrintLog(_("* ecc md5sum       : %s (BAD)\n"),digest);
           if(Closure->guiMode)
	   {  SetLabelText(GTK_LABEL(wl->cmpEccMd5Sum), "<span color=\"red\">%s</span>", digest);
	      if(!ecc_advice)
		ecc_advice = g_strdup(_("<span color=\"red\">Damaged error correction data - try data recovery anyways!</span>"));
	   }
      }
   }
   else
   {  PrintLog(_("- ecc md5sum       : %s\n"), "-");
     
      if(Closure->guiMode)
	SetLabelText(GTK_LABEL(wl->cmpEccMd5Sum), "%s", "-");
   }


   /*** Print final results */

   if(Closure->guiMode)
   {  if(ecc_advice) 
      {  SetLabelText(GTK_LABEL(wl->cmpEccResult), ecc_advice);
         g_free(ecc_advice);
      }
      else 
	if(!crc_missing && !ecc_missing && !hdr_missing && !hdr_crc_errors)
	  SetLabelText(GTK_LABEL(wl->cmpEccResult),
		       _("<span color=\"#008000\">Good error correction data.</span>"));
        else
	  SetLabelText(GTK_LABEL(wl->cmpEccResult),
		       _("<span color=\"red\">Damaged error correction data - try data recovery anyways!</span>"));
   }

   /*** Close and clean up */

terminate:
   cleanup((gpointer)cc);
}
