/*Daala video codec
Copyright (c) 2002-2015 Daala project contributors.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/dcbuffer.h>
#include <wx/tokenzr.h>

#include <ogg/ogg.h>

#include "daala/codec.h"
#include "daala/daaladec.h"

/*Smallest blocks are 4x4*/
#define OD_LOG_BSIZE0 (2)
/*There are 5 block sizes total (4x4, 8x8, 16x16, 32x32 and 64x64).*/
#define OD_NBSIZES (5)
/*The log of the maximum length of the side of a block.*/
#define OD_LOG_BSIZE_MAX (OD_LOG_BSIZE0 + OD_NBSIZES - 1)
/*The maximum length of the side of a block.*/
#define OD_BSIZE_MAX (1 << OD_LOG_BSIZE_MAX)
/*The maximum number of quad tree levels when splitting a super block.*/
#define OD_MAX_SB_SPLITS (OD_NBSIZES - 1)

/*Note that OD_BLOCK_NXN = log2(N) - 2.*/
#define OD_BLOCK_64X64 (4)

/*Largest motion compensation partition sizes are 64x64.*/
# define OD_LOG_MVBSIZE_MAX (6)
# define OD_MVBSIZE_MAX (1 << OD_LOG_MVBSIZE_MAX)
/*Smallest motion compensation partition sizes are 8x8.*/
# define OD_LOG_MVBSIZE_MIN (3)
# define OD_MVBSIZE_MIN (1 << OD_LOG_MVBSIZE_MIN)

/*The deringing filter is applied on 8x8 blocks, but it's application
   is signaled on a 64x64 grid.*/
#define OD_LOG_DERING_GRID (OD_BLOCK_64X64)

/*The superblock resolution of the block size array.  Because four 4x4 blocks
   and one 8x8 can be resolved with a single entry, this is the maximum number
   of 8x8 blocks that can lie along a superblock edge.*/
#define OD_BSIZE_GRID (1 << (OD_MAX_SB_SPLITS - 1))

/*The number of 4x4 blocks that lie along a superblock edge.*/
#define OD_FLAGS_GRID (1 << OD_MAX_SB_SPLITS)

#define OD_MAXI(a, b) ((a) ^ (((a) ^ (b)) & -((b) > (a))))
#define OD_MINI(a, b) ((a) ^ (((b) ^ (a)) & -((b) < (a))))
#define OD_CLAMPI(a, b, c) (OD_MAXI(a, OD_MINI(b, c)))

#define OD_SIGNMASK(a) (-((a) < 0))
#define OD_FLIPSIGNI(a, b) (((a) + OD_SIGNMASK(b)) ^ OD_SIGNMASK(b))
#define OD_DIV_ROUND(x, y) (((x) + OD_FLIPSIGNI((y) >> 1, x))/(y))

#define OD_BLOCK_SIZE4x4(bsize, bstride, bx, by) \
 ((bsize)[((by) >> 1)*(bstride) + ((bx) >> 1)])

/*Command line flag to enable bit accounting*/
#define OD_BIT_ACCOUNTING_SWITCH "a"

#define OD_DERING_LEVELS (6)
static const char *const OD_DERING_COLOR_NAMES[OD_DERING_LEVELS] = {
  "Green", "Light Blue", "Blue", "Gray", "Pink", "Red"
};
static const double OD_DERING_GAIN_TABLE[OD_DERING_LEVELS] = {
  0, 0.5, 0.707, 1, 1.41, 2
};
static const unsigned char OD_DERING_CR[] = {
  96, 92, 119, 128, 160, 255
};
static const unsigned char OD_DERING_CB[] = {
  96, 255, 160, 128, 128, 128
};

struct od_mv_grid_pt {
  /*The x, y offsets of the motion vector in units of 1/8th pixels.*/
  int mv[2];
  /*The motion vector for backward prediction.*/
  int mv1[2];
  /*Whether or not this MV actually has a valid value.*/
  unsigned valid:1;
  /*The ref image that this MV points into.*/
  /*For P frame, 0:golden frame, 1:previous frame. */
  /*For B frame, 1:previous frame, 2:next frame, 3:both frames.*/
  unsigned ref:3;
};

class DaalaDecoder {
private:
  FILE *input;
  wxString path;

  ogg_page page;
  ogg_sync_state oy;
  ogg_stream_state os;

  daala_info di;
  daala_comment dc;
  daala_setup_info *dsi;
  daala_dec_ctx *dctx;

  bool readPage();
  bool readPacket(ogg_packet *packet);
  bool readHeaders();
public:
  daala_image img;
  int frame;

  DaalaDecoder();
  ~DaalaDecoder();

  bool open(const wxString &path);
  void close();
  bool step();
  void restart();

  int getWidth() const;
  int getHeight() const;

  int getFrameWidth() const;
  int getFrameHeight() const;
  int getRunningFrameCount() const;

  int getNHMVBS() const;
  int getNVMVBS() const;

  bool setBlockSizeBuffer(unsigned char *buf, size_t buf_sz);
  bool setBandFlagsBuffer(unsigned int *buf, size_t buf_sz);
  bool setAccountingEnabled(bool enable);
  bool getAccountingStruct(od_accounting **acct);
  bool setDeringFlagsBuffer(unsigned char *buf, size_t buf_sz);
  bool setMVBuffer(od_mv_grid_pt *buf, size_t buf_sz);
};

static void ogg_to_daala_packet(daala_packet *dp, ogg_packet *op) {
  dp->packet     = op->packet;
  dp->bytes      = op->bytes;

  dp->b_o_s      = op->b_o_s;
  dp->e_o_s      = op->e_o_s;

  dp->granulepos = op->granulepos;
  dp->packetno   = op->packetno;
}

bool DaalaDecoder::readPage() {
  while (ogg_sync_pageout(&oy, &page) != 1) {
    char *buffer = ogg_sync_buffer(&oy, 4096);
    if (buffer == NULL) {
      return false;
    }
    int bytes = fread(buffer, 1, 4096, input);
    // End of file
    if (bytes == 0) {
      return false;
    }
    if (ogg_sync_wrote(&oy, bytes) != 0) {
      return false;
    }
  }
  return true;
}

bool DaalaDecoder::readPacket(ogg_packet *packet) {
  while (ogg_stream_packetout(&os, packet) != 1) {
    if (!readPage()) {
      return false;
    }
    if (ogg_stream_pagein(&os, &page) != 0) {
      return false;
    }
  }
  return true;
}

bool DaalaDecoder::readHeaders() {
  bool done = false;
  while (!done && readPage()) {
    int serial = ogg_page_serialno(&page);
    if (ogg_page_bos(&page)) {
      if (ogg_stream_init(&os, serial) != 0) {
        return false;
      }
    }
    if (ogg_stream_pagein(&os, &page) != 0) {
      return false;
    }
    ogg_packet op;
    while (!done && readPacket(&op) != 0) {
      daala_packet dp;
      ogg_to_daala_packet(&dp, &op);
      int ret = daala_decode_header_in(&di, &dc, &dsi, &dp);
      if (ret < 0) {
        if (memcmp(dp.packet, "fishead", dp.bytes)) {
          fprintf(stderr, "Ogg Skeleton streams not supported\n");
        }
        return false;
      }
      if (ret == 0) {
        done = true;
        dctx = daala_decode_create(&di, dsi);
        if (dctx == NULL) {
          return false;
        }
      }
    }
  }
  return done;
}

DaalaDecoder::DaalaDecoder() : input(NULL), dsi(NULL), dctx(NULL) {
  daala_info_init(&di);
  daala_comment_init(&dc);
}

DaalaDecoder::~DaalaDecoder() {
  close();
}

bool DaalaDecoder::open(const wxString &path) {
  ogg_sync_init(&oy);
  input = fopen(path.mb_str(), "rb");
  if (input == NULL) {
    fprintf(stderr, "Could not find file '%s'.\n", path.mb_str().data());
    return false;
  }
  this->path = path;
  frame = 0;
  return readHeaders();
}

void DaalaDecoder::close() {
  if (input) {
    fclose(input);
    input = NULL;
  }
  if (dsi) {
    daala_setup_free(dsi);
    dsi = NULL;
    ogg_stream_clear(&os);
  }
  if (dctx) {
    daala_decode_free(dctx);
    dctx = NULL;
  }
  ogg_sync_clear(&oy);
  daala_info_clear(&di);
  daala_comment_clear(&dc);
}

bool DaalaDecoder::step() {
  //fprintf(stderr, "reading frame %i\n", frame);
  ogg_packet op;
  daala_packet dp;
  while (!daala_decode_img_out(dctx, &img)) {
    if (!readPacket(&op)) {
      /* Reached end of file */
      return false;
    }
    ogg_to_daala_packet(&dp, &op);
    if (daala_decode_packet_in(dctx, &dp) != OD_SUCCESS) {
      /* Error decoding packet. */
      return false;
    }
  }
  frame++;
  return true;
}

void DaalaDecoder::restart() {
  close();
  daala_info_init(&di);
  daala_comment_init(&dc);
  open(path);
}

int DaalaDecoder::getWidth() const {
  return di.pic_width;
}

int DaalaDecoder::getHeight() const {
  return di.pic_height;
}

int DaalaDecoder::getFrameWidth() const {
  return di.pic_width + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1);
}

int DaalaDecoder::getFrameHeight() const {
  return di.pic_height + (OD_BSIZE_MAX - 1) & ~(OD_BSIZE_MAX - 1);
}

int DaalaDecoder::getRunningFrameCount() const {
  return frame;
}

int DaalaDecoder::getNHMVBS() const {
  return getFrameWidth() >> OD_LOG_MVBSIZE_MIN;
}

int DaalaDecoder::getNVMVBS() const {
  return getFrameHeight() >> OD_LOG_MVBSIZE_MIN;
}

bool DaalaDecoder::setBlockSizeBuffer(unsigned char *buf, size_t buf_sz) {
  if (dctx == NULL) {
    return false;
  }
  return daala_decode_ctl(dctx, OD_DECCTL_SET_BSIZE_BUFFER, buf, buf_sz) ==
   OD_SUCCESS;
}

bool DaalaDecoder::setBandFlagsBuffer(unsigned int *buf, size_t buf_sz) {
  if (dctx == NULL) {
    return false;
  }
  return daala_decode_ctl(dctx, OD_DECCTL_SET_FLAGS_BUFFER, buf, buf_sz) ==
   OD_SUCCESS;
}

bool DaalaDecoder::setAccountingEnabled(bool enable) {
  if (dctx == NULL) {
    return false;
  }
  int e = enable ? 1 : 0;
  return daala_decode_ctl(dctx, OD_DECCTL_SET_ACCOUNTING_ENABLED, &e,
   sizeof(e)) == OD_SUCCESS;
}

bool DaalaDecoder::getAccountingStruct(od_accounting **acct) {
  if (dctx == NULL) {
    return false;
  }
  return
   daala_decode_ctl(dctx, OD_DECCTL_GET_ACCOUNTING, acct, sizeof(acct)) ==
   OD_SUCCESS;
}

bool DaalaDecoder::setDeringFlagsBuffer(unsigned char *buf, size_t buf_sz) {
  if (dctx == NULL) {
    return false;
  }
  return daala_decode_ctl(dctx, OD_DECCTL_SET_DERING_BUFFER, buf, buf_sz) ==
   OD_SUCCESS;
}

bool DaalaDecoder::setMVBuffer(od_mv_grid_pt *buf, size_t buf_sz) {
  if (dctx == NULL) {
    return false;
  }
  /* We set this buffer to zero because the first frame is an I-frame and has
     no motion vectors, yet we allow you to enable MV block visualization. */
  memset(buf, 0, buf_sz);
  return daala_decode_ctl(dctx, OD_DECCTL_SET_MV_BUFFER, buf, buf_sz) ==
   OD_SUCCESS;
}

#define MIN_ZOOM (1)
#define MAX_ZOOM (4)

enum {
  OD_LUMA_MASK = 1 << 0,
  OD_CB_MASK = 1 << 1,
  OD_CR_MASK = 1 << 2,
  OD_ALL_MASK = OD_LUMA_MASK | OD_CB_MASK | OD_CR_MASK
};

class TestPanel : public wxPanel {
  DECLARE_EVENT_TABLE()
private:
  DaalaDecoder dd;

  int zoom;
  unsigned char *pixels;

  unsigned char *bsize;
  unsigned int bsize_len;
  int bstride;
  bool show_blocks;
  bool show_motion;

  unsigned int *flags;
  unsigned int flags_len;
  int fstride;
  bool show_skip;
  bool show_noref;
  bool show_padding;
  bool show_dering;
  int nhsb;
  int nhdr;

  od_accounting *acct;
  const bool bit_accounting;
  bool show_bits;
  wxString show_bits_filter;
  double *bpp_q3;

  unsigned char *dering;
  unsigned int dering_len;

  od_mv_grid_pt *mv;
  unsigned int mv_len;

  int plane_mask;
  const wxString path;

  // The decode size is the picture size or frame size.
  int getDecodeWidth() const;
  int getDecodeHeight() const;

  // The display size is the decode size, scaled by the zoom.
  int getDisplayWidth() const;
  int getDisplayHeight() const;

  bool updateDisplaySize();

  int getBand(int x, int y) const;
  void computeBitsPerPixel();
public:
  TestPanel(wxWindow *parent, const wxString &path,
    const bool bit_accounting);
  ~TestPanel();

  bool open(const wxString &path);
  void close();
  void render();
  bool nextFrame();
  void refresh();
  bool gotoFrame();
  void filterBits();
  void resetFilterBits();
  void restart();

  int getZoom() const;
  bool setZoom(int zoom);

  void setShowBlocks(bool show_blocks);
  void setShowMotion(bool show_motion);
  void setShowSkip(bool show_skip);
  void setShowNoRef(bool show_noref);
  void setShowPadding(bool show_padding);
  void setShowBits(bool show_bits);
  void setShowDering(bool show_dering);
  void setShowPlane(bool show_plane, int mask);

  bool hasPadding();

  void onPaint(wxPaintEvent &event);
  void onIdle(wxIdleEvent &event);
  void onMouseMotion(wxMouseEvent &event);
  void onMouseLeaveWindow(wxMouseEvent &event);
};

BEGIN_EVENT_TABLE(TestPanel, wxPanel)
  EVT_PAINT(TestPanel::onPaint)
  EVT_MOTION(TestPanel::onMouseMotion)
  EVT_LEAVE_WINDOW(TestPanel::onMouseLeaveWindow)
  //EVT_IDLE(TestPanel::onIdle)
END_EVENT_TABLE()

class TestFrame : public wxFrame {
  DECLARE_EVENT_TABLE()
private:
  TestPanel *panel;
  wxMenu *fileMenu;
  wxMenu *viewMenu;
  wxMenu *playbackMenu;
  const bool bit_accounting;
public:
  TestFrame(const bool bit_accounting);

  void onOpen(wxCommandEvent &event);
  void onClose(wxCommandEvent &event);
  void onQuit(wxCommandEvent &event);
  void onZoomIn(wxCommandEvent &event);
  void onZoomOut(wxCommandEvent &event);
  void onActualSize(wxCommandEvent &event);
  void onToggleViewMenuCheckBox(wxCommandEvent &event);
  void onToggleBlocks(wxCommandEvent &event);
  void onResetAndToggleViewMenuCheckBox(wxCommandEvent &event);
  void onFilterBits(wxCommandEvent &event);
  void onViewReset(wxCommandEvent &event);
  void onNextFrame(wxCommandEvent &event);
  void onGotoFrame(wxCommandEvent &event);
  void onRestart(wxCommandEvent &event);
  void onAbout(wxCommandEvent &event);

  bool open(const wxString &path);
  bool setZoom(int zoom);
  void updateViewMenu();
};

enum {
  wxID_SHOW_BLOCKS = 6000,
  wxID_SHOW_MOTION,
  wxID_SHOW_SKIP,
  wxID_SHOW_NOREF,
  wxID_SHOW_PADDING,
  wxID_SHOW_BITS,
  wxID_FILTER_BITS,
  wxID_SHOW_DERING,
  wxID_SHOW_Y,
  wxID_SHOW_U,
  wxID_SHOW_V,
  wxID_VIEW_RESET,
  wxID_NEXT_FRAME,
  wxID_GOTO_FRAME,
  wxID_RESTART,
  wxID_ACTUAL_SIZE
};

BEGIN_EVENT_TABLE(TestFrame, wxFrame)
  EVT_MENU(wxID_OPEN, TestFrame::onOpen)
  EVT_MENU(wxID_CLOSE, TestFrame::onClose)
  EVT_MENU(wxID_EXIT, TestFrame::onQuit)
  EVT_MENU(wxID_ZOOM_IN, TestFrame::onZoomIn)
  EVT_MENU(wxID_ZOOM_OUT, TestFrame::onZoomOut)
  EVT_MENU(wxID_ACTUAL_SIZE, TestFrame::onActualSize)
  EVT_MENU(wxID_SHOW_BLOCKS, TestFrame::onToggleBlocks)
  EVT_MENU(wxID_SHOW_MOTION, TestFrame::onToggleBlocks)
  EVT_MENU(wxID_SHOW_SKIP, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_SHOW_NOREF, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_SHOW_PADDING, TestFrame::onToggleViewMenuCheckBox)
  EVT_MENU(wxID_SHOW_BITS, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_FILTER_BITS, TestFrame::onFilterBits)
  EVT_MENU(wxID_SHOW_DERING, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_SHOW_Y, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_SHOW_U, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_SHOW_V, TestFrame::onResetAndToggleViewMenuCheckBox)
  EVT_MENU(wxID_VIEW_RESET, TestFrame::onViewReset)
  EVT_MENU(wxID_NEXT_FRAME, TestFrame::onNextFrame)
  EVT_MENU(wxID_GOTO_FRAME, TestFrame::onGotoFrame)
  EVT_MENU(wxID_RESTART, TestFrame::onRestart)
  EVT_MENU(wxID_ABOUT, TestFrame::onAbout)
END_EVENT_TABLE()

TestPanel::TestPanel(wxWindow *parent, const wxString &path,
 const bool bit_accounting) : wxPanel(parent), pixels(NULL), zoom(0),
 bsize(NULL), bsize_len(0), show_blocks(false), show_motion(false),
 flags(NULL), flags_len(0), show_skip(false), show_noref(false),
 show_padding(false), show_dering(false), acct(NULL), show_bits(false),
 show_bits_filter(_("")), bpp_q3(NULL), dering(NULL), dering_len(0),
 plane_mask(OD_ALL_MASK), path(path), bit_accounting(bit_accounting) {
}

TestPanel::~TestPanel() {
  close();
}

bool TestPanel::open(const wxString &path) {
  if (!dd.open(path)) {
    return false;
  }
  if (!setZoom(MIN_ZOOM)) {
    return false;
  }
  nhsb = dd.getFrameWidth() >> OD_LOG_BSIZE_MAX;
  int nvsb = dd.getFrameHeight() >> OD_LOG_BSIZE_MAX;
  nhdr = dd.getFrameWidth() >> (OD_LOG_DERING_GRID + OD_LOG_BSIZE0);
  int nvdr = dd.getFrameHeight() >> (OD_LOG_DERING_GRID + OD_LOG_BSIZE0);
  bsize_len = sizeof(*bsize)*nhsb*OD_BSIZE_GRID*nvsb*OD_BSIZE_GRID;
  bsize = (unsigned char *)malloc(bsize_len);
  if (bsize == NULL) {
    bsize_len = 0;
    close();
    return false;
  }
  bstride = nhsb*OD_BSIZE_GRID;
  if (!dd.setBlockSizeBuffer(bsize, bsize_len)) {
    close();
    return false;
  }
  flags_len = sizeof(*flags)*nhsb*OD_FLAGS_GRID*nvsb*OD_FLAGS_GRID;
  flags = (unsigned int *)malloc(flags_len);
  if (flags == NULL) {
    flags_len = 0;
    fprintf(stderr, "Could not allocate memory\n");
    close();
    return false;
  }
  fstride = nhsb*OD_FLAGS_GRID;
  if (!dd.setBandFlagsBuffer(flags, flags_len)) {
    fprintf(stderr, "Could not set flags buffer\n");
    close();
    return false;
  }
  if (bit_accounting) {
    bpp_q3 =
     (double *)malloc(sizeof(*bpp_q3)*dd.getFrameWidth()*dd.getFrameHeight());
    if (bpp_q3 == NULL) {
      fprintf(stderr, "Could not allocate memory for bit accounting\n");
      close();
      return false;
    }
    if (!dd.setAccountingEnabled(true)) {
      fprintf(stderr, "Could not enable accounting\n");
      close();
      return false;
    }
    if (!dd.getAccountingStruct(&acct)) {
      fprintf(stderr, "Could not get accounting struct\n");
      close();
      return false;
    }
  }
  dering_len = nhdr*nvdr;
  dering = (unsigned char *)malloc(dering_len);
  if (dering == NULL) {
    fprintf(stderr, "Could not allocate memory for deringing buffer\n");
    close();
    return false;
  }
  if (!dd.setDeringFlagsBuffer(dering, dering_len)) {
    fprintf(stderr, "Could not set dering flags buffer\n");
    close();
    return false;
  }
  mv_len = sizeof(od_mv_grid_pt)*(dd.getNHMVBS() + 1)*(dd.getNVMVBS() + 1);
  mv = (od_mv_grid_pt *)malloc(mv_len);
  if (!dd.setMVBuffer(mv, mv_len)) {
    fprintf(stderr, "Could not set mv buffer\n");
    close();
    return false;
  }
  if (!nextFrame()) {
    close();
    return false;
  }
  SetFocus();
  return true;
}

void TestPanel::close() {
  dd.close();
  free(pixels);
  pixels = NULL;
  free(bsize);
  bsize = NULL;
  free(flags);
  flags = NULL;
  free(bpp_q3);
  bpp_q3 = NULL;
  free(dering);
  dering = NULL;
  free(mv);
  mv = NULL;
}

int TestPanel::getDecodeWidth() const {
  return show_padding ? dd.getFrameWidth() : dd.getWidth();
}

int TestPanel::getDecodeHeight() const {
  return show_padding ? dd.getFrameHeight() : dd.getHeight();
}

int TestPanel::getDisplayWidth() const {
  return zoom*getDecodeWidth();
}

int TestPanel::getDisplayHeight() const {
  return zoom*getDecodeHeight();
}

int TestPanel::getBand(int x, int y) const {
  if (x == 0 && y == 0) return -1;
  if (x < 4 && y < 4) return 0;
  if (x < 8 && y < 2) return 1;
  if (x < 2 && y < 8) return 2;
  if (x < 8 && y < 8) return 3;
  if (x < 16 && y < 4) return 4;
  if (x < 4 && y < 16) return 5;
  if (x < 16 && y < 16) return 6;
  if (x < 32 && y < 8) return 7;
  if (x < 8 && y < 32) return 8;
  if (x < 32 && y < 32) return 9;
  if (x < 64 && y < 16) return 10;
  if (x < 16 && y < 64) return 11;
  return 12;
}

ogg_int64_t block_edge_luma(ogg_int64_t yval) {
  return yval > 50 ? yval >> 1 : yval + 15;
}

void TestPanel::render() {
  daala_image *img = &dd.img;
  /* Assume both chroma planes are decimated the same */
  int xdec = img->planes[1].xdec;
  int ydec = img->planes[1].ydec;
  int y_stride = img->planes[0].ystride;
  int cb_stride = img->planes[1].ystride;
  int cr_stride = img->planes[2].ystride;
  int p_stride = 3*getDisplayWidth();
  unsigned char *y_row = img->planes[0].data;
  unsigned char *cb_row = img->planes[1].data;
  unsigned char *cr_row = img->planes[2].data;
  unsigned char *p_row = pixels;
  double norm;
  if (show_bits) {
    double maxval = 0;
    for (int j = 0; j < getDecodeHeight(); j++) {
      for (int i = 0; i < getDecodeWidth(); i++) {
        double bpp = bpp_q3[j*dd.getFrameWidth() + i];
        if (bpp > maxval) maxval = bpp;
      }
    }
    norm = 1./(1e-4+maxval);
  }

  for (int j = 0; j < getDecodeHeight(); j++) {
    unsigned char *y = y_row;
    unsigned char *cb = cb_row;
    unsigned char *cr = cr_row;
    unsigned char *p = p_row;
    for (int i = 0; i < getDecodeWidth(); i++) {
      ogg_int64_t yval;
      ogg_int64_t cbval;
      ogg_int64_t crval;
      unsigned rval;
      unsigned gval;
      unsigned bval;
      int pmask;
      yval = *y;
      cbval = *cb;
      crval = *cr;
      pmask = plane_mask;
      if (show_skip || show_noref) {
        unsigned char d = OD_BLOCK_SIZE4x4(bsize, bstride, i >> 2, j >> 2);
        int band = getBand(i & ((1 << (d + 2)) - 1), j & ((1 << (d + 2)) - 1));
        int bx = i & ~((1 << (d + 2)) - 1);
        int by = j & ~((1 << (d + 2)) - 1);
        unsigned int flag = flags[fstride*(by >> 2) + (bx >> 2)];
        cbval = 128;
        crval = 128;
        pmask = OD_ALL_MASK;
        if (band >= 0) {
          /*R: U=84, V=255, B: U=255, V=107, G: U=43, V=21*/
          bool skip = (flag >> 2*band)&1;
          bool noref = (flag >> (2*band + 1)) & 1;
          if (skip && show_skip && noref && show_noref) {
            cbval = 43;
            crval = 21;
          }
          if ((!skip || !show_skip) && noref && show_noref) {
            cbval = 84;
            crval = 255;
          }
          if (skip && show_skip && (!noref || !show_noref)) {
            cbval = 255;
            crval = 107;
          }
        }
      }
      if (show_bits) {
        double bpp = sqrt(bpp_q3[j*dd.getFrameWidth() + i]*norm);
#if 1 /* Make this zero for an alternate colormap. */
        double theta = 2*M_PI*bpp;
        double radius = 1.2*sqrt(bpp);
        cbval = 128 + (int64_t)(127*radius*cos(theta));
        crval = 128 - (int64_t)(127*radius*sin(theta));
        if (cbval < 0) cbval = 0;
        if (cbval > 255) cbval = 255;
        if (crval < 0) crval = 0;
        if (crval > 255) crval = 255;
#else
        bpp *= 9;
        if (bpp < 2) {
          cbval = 128 + (int64_t)(63*bpp);
          crval = 128 - (int64_t)(63*bpp);
        }
        else if (bpp < 4) {
          bpp -= 2;
          cbval = 255 - (int64_t)(127*bpp);
          crval = 0;
        }
        else if (bpp < 6) {
          bpp -= 4;
          cbval = 0;
          crval = (int64_t)(127*bpp);
        }
        else if (bpp < 8) {
          bpp -= 6;
          cbval = (int64_t)(127*bpp);
          crval = 255;
        }
        else if (bpp < 9) {
          bpp -= 8;
          cbval = 255;
          crval = 255 - (int64_t)(127*bpp);
        }
#endif
      }
      if (show_dering) {
        int sbx;
        int sby;
        sbx = i >> (OD_LOG_DERING_GRID + OD_LOG_BSIZE0);
        sby = j >> (OD_LOG_DERING_GRID + OD_LOG_BSIZE0);
        crval = OD_DERING_CR[dering[sby*nhdr + sbx]];
        cbval = OD_DERING_CB[dering[sby*nhdr + sbx]];
      }
      if (show_blocks) {
        unsigned char d = OD_BLOCK_SIZE4x4(bsize, bstride, i >> 2, j >> 2);
        int mask = (1 << (d + OD_LOG_BSIZE0)) - 1;
        if (!(i & mask) || !(j & mask)) {
          yval = block_edge_luma(yval);
          cbval = (cbval + 128) >> 1;
          crval = (crval + 128) >> 1;
          pmask = OD_ALL_MASK;
        }
      }
      if (show_motion) {
        int mask = ~(OD_MVBSIZE_MIN - 1);
        int b = OD_LOG_MVBSIZE_MIN;
        while (i == (i & mask) || j == (j & mask)) {
          mask <<= 1;
          int mid_step = 1 << b++;
          int row = ((i & mask) + mid_step) >> OD_LOG_MVBSIZE_MIN;
          int col = ((j & mask) + mid_step) >> OD_LOG_MVBSIZE_MIN;
          int index = col * (dd.getNHMVBS() + 1) + row;
          if (mv[index].valid) {
            yval = block_edge_luma(yval);
            cbval = 255;
            break;
          }
          if (b > OD_LOG_MVBSIZE_MAX) {
            break;
          }
        }
      }
      if (i == dd.getWidth() || j == dd.getHeight()) {
        /* Display a checkerboard pattern at the padding edge */
        yval = 255 * ((i + j) & 1);
        pmask = OD_ALL_MASK;
      }
      if (pmask & OD_LUMA_MASK) {
        yval -= 16;
      }
      else {
        yval = 128;
      }
      cbval = ((pmask & OD_CB_MASK) >> 1) * (cbval - 128);
      crval = ((pmask & OD_CR_MASK) >> 2) * (crval - 128);
      /*This is intentionally slow and very accurate.*/
      rval = OD_CLAMPI(0, (ogg_int32_t)OD_DIV_ROUND(
       2916394880000LL*yval + 4490222169144LL*crval, 9745792000LL), 65535);
      gval = OD_CLAMPI(0, (ogg_int32_t)OD_DIV_ROUND(
       2916394880000LL*yval - 534117096223LL*cbval - 1334761232047LL*crval,
       9745792000LL), 65535);
      bval = OD_CLAMPI(0, (ogg_int32_t)OD_DIV_ROUND(
       2916394880000LL*yval + 5290866304968LL*cbval, 9745792000LL), 65535);
      unsigned char *px_row = p;
      for (int v = 0; v < zoom; v++) {
        unsigned char *px = px_row;
        for (int u = 0; u < zoom; u++) {
          *(px + 0) = (unsigned char)(rval >> 8);
          *(px + 1) = (unsigned char)(gval >> 8);
          *(px + 2) = (unsigned char)(bval >> 8);
          px += 3;
        }
        px_row += p_stride;
      }
      int dc = ((y - y_row) & 1) | (1 - xdec);
      y++;
      cb += dc;
      cr += dc;
      p += zoom*3;
    }
    int dc = -((j & 1) | (1 - ydec));
    y_row += y_stride;
    cb_row += dc & cb_stride;
    cr_row += dc & cr_stride;
    p_row += zoom*p_stride;
  }
}

int TestPanel::getZoom() const {
  return zoom;
}

bool TestPanel::updateDisplaySize() {
  unsigned char *p =
   (unsigned char *)malloc(sizeof(*p)*3*getDisplayWidth()*getDisplayHeight());
  if (p == NULL) {
    return false;
  }
  free(pixels);
  pixels = p;
  SetSize(getDisplayWidth(), getDisplayHeight());
  return true;
}

bool TestPanel::setZoom(int z) {
  if (z <= MAX_ZOOM && z >= MIN_ZOOM && zoom != z) {
    int old_zoom = zoom;
    zoom = z;
    if (!updateDisplaySize()) {
      zoom = old_zoom;
      return false;
    }
    return true;
  }
  return false;
}

void TestPanel::setShowBlocks(bool show_blocks) {
  this->show_blocks = show_blocks;
}

void TestPanel::setShowMotion(bool show_motion) {
  this->show_motion = show_motion;
}

void TestPanel::setShowSkip(bool show_skip) {
  this->show_skip = show_skip;
}

void TestPanel::setShowPadding(bool show_padding) {
  bool old_show_padding = show_padding;
  this->show_padding = show_padding;
  if (!updateDisplaySize()) {
    this->show_padding = old_show_padding;
  }
}

void TestPanel::setShowBits(bool show_bits) {
  this->show_bits = show_bits;
}

void TestPanel::setShowDering(bool show_dering) {
  this->show_dering = show_dering;
  if (show_dering) {
    fprintf(stderr, "Dering Colormap: ");
    for (int c = 0; c < OD_DERING_LEVELS; c++) {
      fprintf(stderr, "%s -> %0.3f ", OD_DERING_COLOR_NAMES[c],
        OD_DERING_GAIN_TABLE[c]);
    }
    fprintf(stderr, "\n");
  }
}

void TestPanel::setShowPlane(bool show_plane, int mask) {
  if (show_plane) {
    plane_mask |= mask;
  }
  else {
    plane_mask &= ~mask;
  }
}

bool TestPanel::hasPadding() {
  return dd.getFrameWidth() > dd.getWidth() ||
    dd.getFrameHeight() > dd.getHeight();
}

void TestPanel::setShowNoRef(bool show_noref) {
  this->show_noref = show_noref;
}

void TestPanel::computeBitsPerPixel() {
  int i, j;
  double bpp_total;
  double bits_total;
  double bits_filtered;
  static double last_bits_total;
  static double last_bits_filtered;
  int totals_q3[MAX_SYMBOL_TYPES] = {0};
  for (j = 0; j < dd.getFrameHeight(); j++) {
    for (i = 0; i < dd.getFrameWidth(); i++) {
      bpp_q3[j*dd.getFrameWidth() + i] = 0;
    }
  }
  if (show_bits_filter.length()) {
    fprintf(stderr, "Filtering: %s\n",
     (const char*)show_bits_filter.mb_str());
  }
  bpp_total = 0;
  bits_total = 0;
  bits_filtered = 0;
  for (i = 0; i < acct->nb_syms; i++) {
    od_acct_symbol *s;
    s = &acct->syms[i];
    bits_total += s->bits_q3;
    /* Filter */
    wxString key(acct->dict.str[s->id], wxConvUTF8);
    if (show_bits_filter.length()) {
      bool filter = false;
      wxStringTokenizer tokenizer(show_bits_filter, _(","));
      while (tokenizer.HasMoreTokens()) {
        wxString token = tokenizer.GetNextToken();
        if (key.Find(token) >= 0) {
          filter = true;
        }
      }
      if (!filter) {
        continue;
      }
    }
    bits_filtered += s->bits_q3;
    totals_q3[s->id] += s->bits_q3;
    switch (s->layer) {
      case 0:
      case 1:
      case 2:
      case 3: {
        int n, u, v;
        double bpp;
        n = 1 << (s->level + 2);
        bpp = ((double)s->bits_q3)/(n*n);
        for (v = 0; v < n; v++) {
          for (u = 0; u < n; u++) {
            bpp_q3[dd.getFrameWidth()*((s->y << 2) + u) + ((s->x << 2) + v)] +=
             bpp;
            bpp_total += bpp;
          }
        }
        break;
      }
      case OD_ACCT_MV: {
        if ((s->level & 1) == 0) {
          /* Even-level MVs*/
          int n = 64 >> (s->level/2);
          int x, y;
          int x0;
          int y0;
          int x1;
          int y1;
          double n_4 = 1./(n*n*n*n);
          x0 = 8*s->x - (n - 1);
          x1 = 8*s->x + (n - 1);
          y0 = 8*s->y - (n - 1);
          y1 = 8*s->y + (n - 1);
          if (x0 < 0) x0 = 0;
          if (y0 < 0) y0 = 0;
          if (x1 >= dd.getFrameWidth()) x1 = dd.getFrameWidth() - 1;
          if (y1 >= dd.getFrameHeight()) y1 = dd.getFrameHeight() - 1;
          int bits = ((double)s->bits_q3);
          for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
              double tmp;
              /* We spread the bits as (1-x)*(1-y) like the bilinear blending.
                 FIXME: Do exact normalization when we're on the border of the
                 image. */
              tmp = bits*(n - abs(x - 8*s->x))*(n - abs(y - 8*s->y))*n_4;
              bpp_q3[dd.getFrameWidth()*y + x] += tmp;
              bpp_total += tmp;
            }
          }
        }
        else {
          /* Odd-level MVs. */
          int n = 64 >> ((1 + s->level)/2);
          int x, y;
          int x0;
          int y0;
          int x1;
          int y1;
          double n_2 = 1./((2*n + 1)*(2*n + 1));
          x0 = 8*s->x - (n - 1);
          x1 = 8*s->x + (n - 1);
          y0 = 8*s->y - (n - 1);
          y1 = 8*s->y + (n - 1);
          if (x0 < 0) x0 = 0;
          if (y0 < 0) y0 = 0;
          if (x1 >= dd.getFrameWidth()) x1 = dd.getFrameWidth() - 1;
          if (y1 >= dd.getFrameHeight()) y1 = dd.getFrameHeight() - 1;
          int bits = ((double)s->bits_q3);
          for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
              double tmp;
              /* FIXME: Spread the bits in the same was as the blending instead
                 of as a square. */
              tmp = bits*n_2;
              bpp_q3[dd.getFrameWidth()*y + x] += tmp;
              bpp_total += tmp;
            }
          }

        }
        break;
      }
    }
  }
  fprintf(stderr,
   "=== Frame: %-3i ============= Bits  Total %%   Filt %% ====\n",
    dd.frame - 1);
  j = 0;
  /* Find max total. */
  for (i = 0; i < acct->dict.nb_str; i++) {
    if (totals_q3[i] > totals_q3[j]) {
      j = i;
    }
  }
  if (bits_total) {
    for (i = 0; i < acct->dict.nb_str; i++) {
      if (totals_q3[i]) {
        if (i == j) fprintf(stderr, "\033[1;31m");
        fprintf(stderr, "%20s = %10.3f  %5.2f %%  %5.2f %%\n",
         acct->dict.str[i], (float)totals_q3[i]/8,
          (float)totals_q3[i]/bits_total*100,
           (float)totals_q3[i]/bits_filtered*100);
        if (i == j) fprintf(stderr, "\033[0m");
      }
    }
    fprintf(stderr, "%20s = %10.3f\n",
     "bits_total", (float)bits_total/8);
    fprintf(stderr, "%20s = %10.3f %6.2f %%   delta: %+.3f\n", "bits_filtered",
     bits_filtered/8, bits_filtered/bits_total*100, (bits_filtered -
      last_bits_filtered)/8);
    fprintf(stderr, "%20s = %10.3i\n", "nb_syms", acct->nb_syms);
    fprintf(stderr, "%20s = %10.3f\n", "bpp_total", (float)bpp_total/8);
    last_bits_filtered = bits_filtered;
    last_bits_total = bits_total;
  }
}
void TestPanel::refresh() {
  if (bit_accounting) {
    computeBitsPerPixel();
  }
  render();
  ((TestFrame *)GetParent())->SetTitle(path +
   wxString::Format(_(" (%d,%d) Frame %d - Daala Stream Analyzer"),
   dd.getWidth(), dd.getHeight(), dd.getRunningFrameCount()-1));
}
bool TestPanel::nextFrame() {
  if (dd.step()) {
    /* For now just compute the unfiltered bits per pixel. */
    refresh();
    return true;
  }
  return false;
}

bool TestPanel::gotoFrame() {
  bool toReturn;
  int nframe;
  wxTextEntryDialog dlg(this, _("Jump to which frame?"));
  dlg.SetTextValidator(wxFILTER_NUMERIC);
  if (dlg.ShowModal() == wxID_OK) {
    nframe = wxAtoi(dlg.GetValue());
  }
  else {
    return false;
  }
  if (nframe < dd.frame) {
    restart();
  }
  if(nframe <= 0) {
    return true;
  }
  if(nframe == dd.frame+1) {
    return nextFrame();
  }
  while (nframe >= dd.frame) {
    toReturn = dd.step();
    if (!toReturn) {
      wxMessageBox(_("Error: Video doesn't have that many frames"));
      restart();
      return false;
    }
  }
  refresh();
  return toReturn;
}

void TestPanel::resetFilterBits() {
  if (!show_bits_filter.IsEmpty()) {
    show_bits_filter = _("");
    computeBitsPerPixel();
  }
}

void TestPanel::filterBits() {
  wxTextEntryDialog dlg(this,
   _("Filter: \"skip,pvq\" or \"\" to disable filter."));
  dlg.SetValue(show_bits_filter);
  if (dlg.ShowModal() == wxID_OK) {
    wxString new_bits_filter = dlg.GetValue();
    if (!show_bits_filter.IsSameAs(new_bits_filter)) {
      show_bits_filter = new_bits_filter;
      refresh();
    }
  }
}

void TestPanel::restart() {
  dd.restart();
  dd.setBlockSizeBuffer(bsize, bsize_len);
  dd.setBandFlagsBuffer(flags, flags_len);
  if (bit_accounting) {
    dd.setAccountingEnabled(true);
    dd.getAccountingStruct(&acct);
  }
  dd.setDeringFlagsBuffer(dering, dering_len);
  dd.setMVBuffer(mv, mv_len);
  nextFrame();
}

void TestPanel::onMouseMotion(wxMouseEvent& event) {
  const wxPoint pt = wxGetMousePosition();
  int mouse_x = pt.x - this->GetScreenPosition().x;
  int mouse_y = pt.y - this->GetScreenPosition().y;
  TestFrame *parent = static_cast<TestFrame*>(GetParent());
  int row = mouse_y/zoom;
  int col = mouse_x/zoom;
  if (row >= 0 && col >= 0 && row < getDecodeHeight()
   && col < getDecodeWidth()) {
    const daala_image_plane *planes = dd.img.planes;
    /* Assume both chroma planes are decimated the same */
    int xdec = planes[1].xdec;
    int ydec = planes[1].ydec;
    int cb_stride = planes[1].ystride;
    int cr_stride = planes[2].ystride;
    ogg_int64_t y = planes[0].data[planes[0].ystride*row + col];
    ogg_int64_t cb = planes[1].data[cb_stride*(row >> ydec) + (col >> xdec)];
    ogg_int64_t cr = planes[2].data[cr_stride*(row >> ydec) + (col >> xdec)];
    parent->SetStatusText(wxString::Format(_("Y:%lld,U:%lld,V:%lld"),
     y, cb, cr), 2);
    if (show_dering) {
      int sbx;
      int sby;
      sbx = col >> (OD_LOG_DERING_GRID + OD_LOG_BSIZE0);
      sby = row >> (OD_LOG_DERING_GRID + OD_LOG_BSIZE0);

      parent->SetStatusText(wxString::Format(_("Dering:%0.3f"),
       OD_DERING_GAIN_TABLE[dering[sby*nhdr + sbx]]), 1);
    }
    else if (show_bits) {
      parent->SetStatusText(wxString::Format(_("bpp:%0.1f"),
       bpp_q3[row*dd.getFrameWidth() + col]), 1);
    }
    else {
      parent->SetStatusText(_(""), 1);
    }
  }
  else {
    parent->SetStatusText(wxString::Format(_("")), 1);
  }
  parent->SetStatusText(wxString::Format(_("X:%d,Y:%d"),
   col, row), 3);
}

void TestPanel::onMouseLeaveWindow(wxMouseEvent& event) {
  TestFrame *parent = static_cast<TestFrame*>(GetParent());
  parent->SetStatusText(wxString::Format(_("")), 3);
}

void TestPanel::onPaint(wxPaintEvent &) {
  wxBitmap bmp(wxImage(getDisplayWidth(), getDisplayHeight(), pixels, true));
  wxBufferedPaintDC dc(this, bmp);
}

void TestPanel::onIdle(wxIdleEvent &) {
  nextFrame();
  Refresh(false);
  /*wxMilliSleep(input.video_fps_n*1000/input.video_fps_n);*/
}

TestFrame::TestFrame(const bool bit_accounting) : wxFrame(NULL, wxID_ANY,
 _("Daala Stream Analyzer"), wxDefaultPosition, wxDefaultSize,
 wxDEFAULT_FRAME_STYLE), panel(NULL), bit_accounting(bit_accounting) {
  wxMenuBar *mb = new wxMenuBar();

  wxAcceleratorEntry entries[2];
  entries[0].Set(wxACCEL_CTRL, (int)'=', wxID_ZOOM_IN);
  entries[1].Set(wxACCEL_CTRL|wxACCEL_SHIFT, (int)'-', wxID_ZOOM_OUT);
  wxAcceleratorTable accel(2, entries);
  this->SetAcceleratorTable(accel);

  fileMenu = new wxMenu();
  fileMenu->Append(wxID_OPEN, _("&Open...\tCtrl-O"), _("Open daala file"));
  fileMenu->Append(wxID_CLOSE, _("&Close\tCtrl-W"), _("Close daala file"));
  fileMenu->Enable(wxID_CLOSE, false);
  fileMenu->Append(wxID_EXIT, _("E&xit\tCtrl-Q"), _("Quit this program"));
  mb->Append(fileMenu, _("&File"));

  viewMenu = new wxMenu();
  viewMenu->Append(wxID_ZOOM_IN, _("Zoom-In\tCtrl-+"),
   _("Double image size"));
  viewMenu->Append(wxID_ZOOM_OUT, _("Zoom-Out\tCtrl--"),
   _("Half image size"));
  viewMenu->Append(wxID_ACTUAL_SIZE, _("Actual size\tCtrl-0"),
   _("Actual size of the frame"));
  viewMenu->AppendSeparator();
  viewMenu->AppendCheckItem(wxID_SHOW_MOTION,
   _("&MC Blocks\tCtrl-M"),
   _("Show motion-compensation block sizes"));
  viewMenu->AppendCheckItem(wxID_SHOW_BLOCKS, _("&Transform Blocks\tCtrl-B"),
   _("Show transform block sizes"));
  viewMenu->AppendSeparator();
  viewMenu->AppendCheckItem(wxID_SHOW_PADDING, _("&Padding\tCtrl-P"),
   _("Show padding area"));
  viewMenu->AppendCheckItem(wxID_SHOW_SKIP, _("&Skip\tCtrl-S"),
   _("Show skip bands overlay"));
  viewMenu->AppendCheckItem(wxID_SHOW_NOREF, _("&No-Ref\tCtrl-N"),
   _("Show no-ref bands overlay"));
  viewMenu->AppendSeparator();
  viewMenu->AppendCheckItem(wxID_SHOW_DERING, _("&Deringing\tCtrl-D"),
   _("Show deringing filter"));
  viewMenu->AppendSeparator();
  viewMenu->AppendCheckItem(wxID_SHOW_BITS, _("Bit &Accounting\tCtrl-A"),
   _("Show bit accounting"));
  viewMenu->Append(wxID_FILTER_BITS, _("&Filter Bits\tCtrl-F"),
   _("Filter bit accounting"));
  viewMenu->AppendSeparator();
  viewMenu->AppendCheckItem(wxID_SHOW_Y, _("&Y plane\tCtrl-Y"),
   _("Show Y plane"));
  viewMenu->AppendCheckItem(wxID_SHOW_U, _("&U plane\tCtrl-U"),
   _("Show U plane"));
  viewMenu->AppendCheckItem(wxID_SHOW_V, _("&V plane\tCtrl-V"),
   _("Show V plane"));
  viewMenu->AppendSeparator();
  viewMenu->Append(wxID_VIEW_RESET, _("Reset view\tBACK"),
    _("Reset view settings"));

  mb->Append(viewMenu, _("&View"));

  playbackMenu = new wxMenu();
  playbackMenu->Append(wxID_NEXT_FRAME, _("Next frame\tCtrl-."),
   _("Go to next frame"));
  playbackMenu->Append(wxID_RESTART, _("&Restart\tCtrl-R"),
   _("Set video to frame 0"));
  playbackMenu->Append(wxID_GOTO_FRAME, _("Jump to Frame\tCtrl-J"),
   _("Go to frame number"));
  mb->Append(playbackMenu, _("&Playback"));

  wxMenu *helpMenu=new wxMenu();
  helpMenu->Append(wxID_ABOUT, _("&About...\tF1"), _("Show about dialog"));
  mb->Append(helpMenu, _("&Help"));

  SetMenuBar(mb);
  mb->EnableTop(1, false);
  mb->EnableTop(2, false);

  CreateStatusBar(4);
  int status_widths[4] = {-1, 80, 130, 110};
  SetStatusWidths(4, status_widths);
  SetStatusText(_("another day, another daala"));
  GetMenuBar()->Check(wxID_SHOW_Y, true);
  GetMenuBar()->Check(wxID_SHOW_U, true);
  GetMenuBar()->Check(wxID_SHOW_V, true);
  if (!bit_accounting) {
    GetMenuBar()->Enable(wxID_SHOW_BITS, false);
    GetMenuBar()->Enable(wxID_FILTER_BITS, false);
  }
}

void TestFrame::onOpen(wxCommandEvent& WXUNUSED(event)) {
  wxFileDialog openFileDialog(this, _("Open file"), wxEmptyString,
   wxEmptyString, _("Daala files (*.ogv)|*.ogv"),
   wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (openFileDialog.ShowModal() != wxID_CANCEL) {
    open(openFileDialog.GetPath());
  }
}

void TestFrame::onClose(wxCommandEvent &WXUNUSED(event)) {
}

void TestFrame::onQuit(wxCommandEvent &WXUNUSED(event)) {
  Close(true);
}

void TestFrame::onZoomIn(wxCommandEvent &WXUNUSED(event)) {
  setZoom(panel->getZoom() + 1);
}

void TestFrame::onZoomOut(wxCommandEvent &WXUNUSED(event)) {
  setZoom(panel->getZoom() - 1);
}

void TestFrame::onActualSize(wxCommandEvent &WXUNUSED(event)) {
  setZoom(MIN_ZOOM);
}

bool TestFrame::setZoom(int zoom) {
  if (panel->setZoom(zoom)) {
    GetMenuBar()->Enable(wxID_ACTUAL_SIZE, zoom != MIN_ZOOM);
    GetMenuBar()->Enable(wxID_ZOOM_IN, zoom != MAX_ZOOM);
    GetMenuBar()->Enable(wxID_ZOOM_OUT, zoom != MIN_ZOOM);
    SetClientSize(panel->GetSize());
    panel->render();
    panel->Refresh();
    return true;
  }
  return false;
}

void TestFrame::onToggleBlocks(wxCommandEvent &event) {
  GetMenuBar()->Check(wxID_SHOW_BLOCKS, false);
  GetMenuBar()->Check(wxID_SHOW_MOTION, false);
  onToggleViewMenuCheckBox(event);
}

void TestFrame::onToggleViewMenuCheckBox(wxCommandEvent &event) {
  GetMenuBar()->Check(event.GetId(), event.IsChecked());
  updateViewMenu();
}

void TestFrame::onResetAndToggleViewMenuCheckBox(wxCommandEvent &event) {
  GetMenuBar()->Check(wxID_SHOW_BITS, false);
  GetMenuBar()->Check(wxID_SHOW_DERING, false);
  int id = event.GetId();
  if (id != wxID_SHOW_NOREF && id != wxID_SHOW_SKIP) {
    GetMenuBar()->Check(wxID_SHOW_NOREF, false);
    GetMenuBar()->Check(wxID_SHOW_SKIP, false);
  }
  if (id != wxID_SHOW_Y && id != wxID_SHOW_U && id != wxID_SHOW_V) {
    GetMenuBar()->Check(wxID_SHOW_Y, true);
    GetMenuBar()->Check(wxID_SHOW_U, true);
    GetMenuBar()->Check(wxID_SHOW_V, true);
  }
  onToggleViewMenuCheckBox(event);
}

void TestFrame::updateViewMenu() {
  panel->setShowBlocks(GetMenuBar()->IsChecked(wxID_SHOW_BLOCKS));
  panel->setShowMotion(GetMenuBar()->IsChecked(wxID_SHOW_MOTION));
  panel->setShowSkip(GetMenuBar()->IsChecked(wxID_SHOW_SKIP));
  panel->setShowNoRef(GetMenuBar()->IsChecked(wxID_SHOW_NOREF));
  panel->setShowPadding(GetMenuBar()->IsChecked(wxID_SHOW_PADDING));
  panel->setShowBits(GetMenuBar()->IsChecked(wxID_SHOW_BITS));
  panel->setShowDering(GetMenuBar()->IsChecked(wxID_SHOW_DERING));
  panel->setShowPlane(GetMenuBar()->IsChecked(wxID_SHOW_Y), OD_LUMA_MASK);
  panel->setShowPlane(GetMenuBar()->IsChecked(wxID_SHOW_U), OD_CB_MASK);
  panel->setShowPlane(GetMenuBar()->IsChecked(wxID_SHOW_V), OD_CR_MASK);
  SetClientSize(panel->GetSize());
  panel->render();
  panel->Refresh(false);
}

void TestFrame::onViewReset(wxCommandEvent &WXUNUSED(event)) {
  GetMenuBar()->Check(wxID_SHOW_BITS, false);
  GetMenuBar()->Check(wxID_SHOW_DERING, false);
  GetMenuBar()->Check(wxID_SHOW_BLOCKS, false);
  GetMenuBar()->Check(wxID_SHOW_MOTION, false);
  GetMenuBar()->Check(wxID_SHOW_PADDING, false);
  GetMenuBar()->Check(wxID_SHOW_NOREF, false);
  GetMenuBar()->Check(wxID_SHOW_SKIP, false);
  GetMenuBar()->Check(wxID_SHOW_Y, true);
  GetMenuBar()->Check(wxID_SHOW_U, true);
  GetMenuBar()->Check(wxID_SHOW_V, true);
  panel->resetFilterBits();
  updateViewMenu();
}

void TestFrame::onFilterBits(wxCommandEvent &WXUNUSED(event)) {
  panel->filterBits();
  panel->Refresh(false);
}

void TestFrame::onNextFrame(wxCommandEvent &WXUNUSED(event)) {
  panel->nextFrame();
  panel->Refresh(false);
}

void TestFrame::onGotoFrame(wxCommandEvent &WXUNUSED(event)) {
  panel->gotoFrame();
  panel->Refresh(false);
}

void TestFrame::onRestart(wxCommandEvent &WXUNUSED(event)) {
  panel->restart();
  panel->Refresh(false);
}

void TestFrame::onAbout(wxCommandEvent& WXUNUSED(event)) {
  wxMessageBox(_("This program is a bitstream analyzer for Daala."),
   _("About"), wxOK | wxICON_INFORMATION, this);
}

bool TestFrame::open(const wxString &path) {
  panel = new TestPanel(this, path, bit_accounting);
  if (panel->open(path)) {
    GetMenuBar()->Enable(wxID_ACTUAL_SIZE, false);
    GetMenuBar()->Enable(wxID_ZOOM_IN, true);
    GetMenuBar()->Enable(wxID_ZOOM_OUT, false);
    SetClientSize(panel->GetSize());
    panel->Refresh();
    SetStatusText(_("loaded file: ") + path);
    fileMenu->Enable(wxID_OPEN, false);
    viewMenu->Enable(wxID_SHOW_PADDING, panel->hasPadding());
    GetMenuBar()->EnableTop(1, true);
    GetMenuBar()->EnableTop(2, true);
    return true;
  }
  else {
    delete panel;
    panel = NULL;
    SetStatusText(_("error loading file") + path);
    return false;
  }
}

class TestApp : public wxApp {
private:
  TestFrame *frame;
public:
  void OnInitCmdLine(wxCmdLineParser &parser);
  bool OnCmdLineParsed(wxCmdLineParser &parser);
};

static const wxCmdLineEntryDesc CMD_LINE_DESC [] = {
  { wxCMD_LINE_SWITCH, _("h"), _("help"),
   _("Display this help and exit."), wxCMD_LINE_VAL_NONE,
   wxCMD_LINE_OPTION_HELP },
  { wxCMD_LINE_SWITCH, _(OD_BIT_ACCOUNTING_SWITCH), _("bit-accounting"),
   _("Enable bit accounting"), wxCMD_LINE_VAL_NONE,
   wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_PARAM, NULL, NULL, _("input.ogg"), wxCMD_LINE_VAL_STRING,
   wxCMD_LINE_PARAM_OPTIONAL },
  { wxCMD_LINE_NONE }
};

void TestApp::OnInitCmdLine(wxCmdLineParser &parser) {
  parser.SetDesc(CMD_LINE_DESC);
  parser.SetSwitchChars(_("-"));
}

bool TestApp::OnCmdLineParsed(wxCmdLineParser &parser) {
  frame = new TestFrame(parser.Found(_(OD_BIT_ACCOUNTING_SWITCH)));
  frame->Show();
  if (parser.GetParamCount() > 0) {
    return frame->open(parser.GetParam(0));
  }
  return true;
}

IMPLEMENT_APP(TestApp)
