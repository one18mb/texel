// 字画 texel - 极简像素草稿本
// 顶栏: 粗细/颜色/色块   画布: 文字层(下)+笔迹层(上)   底栏: 历史时间轴
// 左键点=定位文字光标, 左键长按/拖动=矩形选中(反色); 右键=画笔(Shift=八向直线)
// Ctrl+C/V 复制粘贴(粘贴算一次"写"); Ctrl+Z/Y 回撤/重做; Ctrl+S 保存; Ctrl+N 新建; Ctrl+L 清空
// Unifont 点阵无抗锯齿，半角 8px / 全角 16px
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <imm.h>
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "resource.h"

using namespace Gdiplus;

static const int      COLW = 8;
static const int      ROWH = 16;
static const uint32_t CONT = 0xFFFFFFFEu;
static const uint32_t CP_WRITE = 0x5199u;
static const uint32_t CP_DRAW  = 0x753Bu;

static HINSTANCE g_hInst = nullptr;
static int      g_W = 50, g_H = 50;
static int      g_cols = 100;
static int      g_pw = 0, g_ph = 0;
static uint32_t g_bg = 0xFFFFFF, g_fg = 0x000000;

static std::vector<uint32_t> g_cells;
static std::vector<uint32_t> g_ink;
static std::vector<uint32_t> g_text;
static std::vector<uint32_t> g_fb;

static uint32_t g_brush = 0xFF000000;
static int      g_size  = 1;
static int      g_cx = 0, g_cy = 0;
static int      g_TB = 24;
static int      g_SB = 17;

static const uint8_t* g_font = nullptr;
static uint32_t       g_fontN = 0;

struct Stroke { uint32_t color; int size; std::vector<POINT> pts; };
static std::vector<Stroke> g_strokes;

// 历史操作：文字用【矩形块】存 before/after，笔迹存点
enum { OP_STROKE = 0, OP_TEXT = 1 };
struct Op {
  int type=OP_STROKE;
  int r0=0,c0=0,rows=0,cols=0;
  std::vector<uint32_t> before, after;
  int cbx0=0,cby0=0,cbx1=0,cby1=0;
  uint32_t color=0; int size=0;
  std::vector<POINT> pts;
};
static std::vector<Op> g_ops;
static int             g_pos = 0;
static std::vector<uint32_t> g_baseCells;   // 基础文字(仍是格子)
static std::vector<uint32_t> g_baseInk;     // 基础笔迹(已栅格化位图)

struct Sw { uint32_t color; int kind; };
static std::vector<Sw> g_sw;
static const uint32_t EGA16[16]={
  0x000000,0x0000AA,0x00AA00,0x00AAAA,
  0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
  0x555555,0x5555FF,0x55FF55,0x55FFFF,
  0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
};

static HWND   g_hwnd = nullptr, g_hEdit = nullptr, g_hEditSize = nullptr;
static HDC    g_memDC = nullptr;
static HBITMAP g_memBmp = nullptr, g_memBmpOld = nullptr;
static uint32_t* g_memBits = nullptr;
static int    g_cw = 0, g_ch = 0;
static WNDPROC g_editProc = nullptr;
static HFONT  g_uiFont = nullptr;
static HFONT  g_uiFontSmall = nullptr;
static bool   g_drawing = false;
static bool   g_dirty   = false;
static int    g_winX = 0, g_winY = 0;
static bool   g_hasPos = false;
static POINT  g_last = {0,0};
static int    g_swatchX0 = 130, g_swatchY0 = 5, g_swPitch = 16, g_swSz = 14;

// 选中
static bool   g_lbtnDown=false, g_selecting=false, g_hasSel=false;
static int    g_selAnchorC=0, g_selAnchorR=0;
static int    g_selC0=0, g_selC1=0, g_selR0=0, g_selR1=0;
static POINT  g_pressPt = {0,0};
static wchar_t g_pendingHigh = 0;   // UTF-16 高代理暂存
static bool   g_skipNew = false;      // ini: SkipNew
static bool   g_noSavePrompt = false; // ini: NoSavePrompt
static const int HELP_X=4, HELP_Y=4, HELP_W=32, HELP_H=16;

static inline uint32_t RGB24(int r,int g,int b){ return (uint32_t)((r<<16)|(g<<8)|b); }
static inline uint32_t argb(uint32_t c){ return 0xFF000000u | (c & 0xFFFFFFu); }
static inline int imin(int a,int b){ return a<b?a:b; }
static inline int imax(int a,int b){ return a>b?a:b; }
static void stampBuf(uint32_t* buf,int cx,int cy,int r,uint32_t col);
static void lineBuf(uint32_t* buf,int x0,int y0,int x1,int y1,int r,uint32_t col);

// ---------------- 字形 ----------------
struct Glyph { const uint8_t* bits; int w; };
static Glyph getGlyph(uint32_t cp){
  Glyph r{nullptr,1};
  uint32_t lo=0, hi=g_fontN;
  while(lo<hi){
    uint32_t mid = lo + ((hi-lo)>>1);
    const uint8_t* e = g_font + (size_t)mid*37;
    uint32_t v = (uint32_t)e[0]|((uint32_t)e[1]<<8)|((uint32_t)e[2]<<16)|((uint32_t)e[3]<<24);
    if(v==cp){ r.bits=e+5; int w=e[4]; r.w=(w==2?2:1); return r; }
    if(v<cp) lo=mid+1; else hi=mid;
  }
  return r;
}

// 选区边界外扩到字形边界：绝不切半个全角字
static void selBounds(int& c0,int& c1,int& r0,int& r1){
  c0=imin(g_selC0,g_selC1); c1=imax(g_selC0,g_selC1);
  r0=imin(g_selR0,g_selR1); r1=imax(g_selR0,g_selR1);
  if(c0<0)c0=0; if(c1>g_cols-1)c1=g_cols-1;
  if(r0<0)r0=0; if(r1>g_H-1)r1=g_H-1;
  for(;;){                                  // 左边界：落在全角右半格 -> 左移整字
    bool need=false;
    for(int r=r0;r<=r1 && !need;r++) if(c0>0 && g_cells[(size_t)r*g_cols+c0]==CONT) need=true;
    if(!need||c0<=0) break;
    c0--;
  }
  for(;;){                                  // 右边界：正好是全角左半格 -> 右移整字
    bool need=false;
    for(int r=r0;r<=r1 && !need;r++){
      uint32_t v=g_cells[(size_t)r*g_cols+c1];
      if(v && v!=CONT){ Glyph gl=getGlyph(v); if(gl.w==2 && c1+1<g_cols) need=true; }
    }
    if(!need||c1>=g_cols-1) break;
    c1++;
  }
}

// UTF-32 码点 <-> UTF-16
static void appendCP(std::wstring& s,uint32_t cp){
  if(cp<=0xFFFF) s.push_back((wchar_t)cp);
  else { cp-=0x10000u; s.push_back((wchar_t)(0xD800+(cp>>10))); s.push_back((wchar_t)(0xDC00+(cp&0x3FF))); }
}
static void decodeCP(const std::wstring& s,std::vector<uint32_t>& out){
  out.clear();
  for(size_t i=0;i<s.size();i++){
    uint32_t c=(uint32_t)s[i];
    if(c>=0xD800&&c<=0xDBFF && i+1<s.size()){
      uint32_t d=(uint32_t)s[i+1];
      if(d>=0xDC00&&d<=0xDFFF){ out.push_back(0x10000u+((c-0xD800u)<<10)+(d-0xDC00u)); i++; continue; }
    }
    out.push_back(c);
  }
}

// ---------------- 合成 ----------------
// 只重排指定行范围的文字层（增量）
static void renderTextRows(int r0,int r1){
  if(r0<0) r0=0; if(r1>g_H-1) r1=g_H-1;
  uint32_t bgg=argb(g_bg), fgg=argb(g_fg);
  for(int y=r0;y<=r1;y++){
    int by=y*ROWH;
    for(int r=0;r<ROWH;r++){ int py=by+r; if(py<0||py>=g_ph) continue;
      uint32_t* p=g_text.data()+(size_t)py*g_pw; for(int x=0;x<g_pw;x++) p[x]=bgg; }
    for(int c=0;c<g_cols;c++){
      uint32_t cp=g_cells[(size_t)y*g_cols+c];
      if(!cp||cp==CONT) continue;
      Glyph gl=getGlyph(cp); if(!gl.bits) continue;
      int bx=c*COLW, maxc=(gl.w==2)?16:8;
      for(int r=0;r<ROWH;r++){
        uint16_t row=(uint16_t)((gl.bits[r*2]<<8)|gl.bits[r*2+1]);
        if(!row) continue;
        int py=by+r; if(py<0||py>=g_ph) continue;
        uint32_t* tline=g_text.data()+(size_t)py*g_pw+bx;
        for(int k=0;k<maxc;k++){ int px=bx+k; if(px>=g_pw) break; if(row&(0x8000>>k)) tline[k]=fgg; }
      }
    }
  }
}
static void renderText(){ renderTextRows(0,g_H-1); }
static void compose(){
  memcpy(g_fb.data(), g_text.data(), (size_t)g_pw*g_ph*4);
  uint32_t* ink=g_ink.data(); uint32_t* fb=g_fb.data();
  size_t n=(size_t)g_pw*g_ph;
  for(size_t i=0;i<n;i++){ uint32_t v=ink[i]; if(v>>24) fb[i]=v; }
}

// ---------------- 脏矩形 + 覆盖层失效 ----------------
static RECT emptyRectPx(){ RECT r={0,0,-1,-1}; return r; }
static bool rectEmpty(const RECT& r){ return r.right<r.left||r.bottom<r.top; }
static void clampPx(RECT& r){
  if(r.left<0)r.left=0; if(r.top<0)r.top=0;
  if(r.right>g_pw-1)r.right=g_pw-1; if(r.bottom>g_ph-1)r.bottom=g_ph-1;
}
static RECT rowsRectPx(int r0,int r1){ RECT r={0,r0*ROWH,g_pw-1,r1*ROWH+ROWH-1}; return r; }
static RECT brushRectPx(int x0,int y0,int x1,int y1,int rad){
  RECT r={imin(x0,x1)-rad,imin(y0,y1)-rad,imax(x0,x1)+rad,imax(y0,y1)+rad}; return r;
}
static void invalidatePx(const RECT& r0){
  RECT r=r0; clampPx(r); if(rectEmpty(r)) return;
  RECT c={r.left,g_TB+r.top,r.right+1,g_TB+r.bottom+1};
  InvalidateRect(g_hwnd,&c,FALSE);
}
static void recomposePx(const RECT& r0){
  RECT r=r0; clampPx(r); if(rectEmpty(r)) return;
  for(int y=r.top;y<=r.bottom;y++){
    size_t base=(size_t)y*g_pw;
    uint32_t* fb=g_fb.data()+base; const uint32_t* tx=g_text.data()+base; const uint32_t* ink=g_ink.data()+base;
    for(int x=r.left;x<=r.right;x++){ uint32_t v=ink[x]; fb[x]=(v>>24)?v:tx[x]; }
  }
}
static void commitRect(const RECT& r){ invalidatePx(r); recomposePx(r); }
static void redrawAllCanvas(){ RECT r={0,0,g_pw-1,g_ph-1}; commitRect(r); }

static RECT caretRectPx(){
  int row=g_cy,col=g_cx,w=1;
  uint32_t cp=g_cells[(size_t)row*g_cols+g_cx];
  if(cp==CONT){col=g_cx-1;w=2;} else if(cp){Glyph gl=getGlyph(cp);w=(gl.w==2)?2:1;}
  RECT r={col*COLW,row*ROWH,col*COLW+w*COLW-1,row*ROWH+ROWH-1}; return r;
}
static RECT selRectPx(){
  RECT r=emptyRectPx();
  if(!g_selecting&&!g_hasSel) return r;
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  r.left=c0*COLW; r.top=r0*ROWH; r.right=(c1+1)*COLW-1; r.bottom=(r1+1)*ROWH-1;
  return r;
}
static void unionR(RECT& a,const RECT& b){
  if(rectEmpty(b)) return;
  if(rectEmpty(a)){ a=b; return; }
  if(b.left<a.left)a.left=b.left; if(b.top<a.top)a.top=b.top;
  if(b.right>a.right)a.right=b.right; if(b.bottom>a.bottom)a.bottom=b.bottom;
}
static RECT overlayRectPx(){ RECT a=selRectPx(); unionR(a,caretRectPx()); return a; }
static RECT g_prevOverlay={0,0,-1,-1};
static void overlayChanged(){
  invalidatePx(g_prevOverlay);
  g_prevOverlay=overlayRectPx();
  invalidatePx(g_prevOverlay);
}
static void invalidateStatus(){ RECT c={0,g_TB+g_ph,g_cw,g_ch}; InvalidateRect(g_hwnd,&c,FALSE); }

// ---------------- 格子 ----------------
static void clearGlyphAt(int row,int col){
  if(col<0||col>=g_cols) return;
  int start=(g_cells[(size_t)row*g_cols+col]==CONT)? col-1 : col;
  if(start<0) return;
  uint32_t cp=g_cells[(size_t)row*g_cols+start];
  if(!cp||cp==CONT) return;
  Glyph gl=getGlyph(cp); int w=(gl.w==2)?2:1;
  g_cells[(size_t)row*g_cols+start]=0;
  if(w==2 && start+1<g_cols) g_cells[(size_t)row*g_cols+start+1]=0;
}
static void truncateFuture(){ if((int)g_ops.size()>g_pos) g_ops.resize(g_pos); }
static int timelineCap(){ int c=g_cw/16; return c<1?1:c; }
static void baseReset(){
  g_baseCells.assign((size_t)g_H*g_cols,0u);
  g_baseInk.assign((size_t)g_pw*g_ph,0u);
}
static void dropOldest(){
  if(g_ops.empty()) return;
  Op op=g_ops.front();
  if(op.type==OP_TEXT){
    for(int r=0;r<op.rows;r++)
      for(int c=0;c<op.cols;c++){
        int rr=op.r0+r, cc=op.c0+c;
        if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols) g_baseCells[(size_t)rr*g_cols+cc]=op.after[(size_t)r*op.cols+c];
      }
  } else {
    int r=op.size/2;
    if(op.pts.size()==1) stampBuf(g_baseInk.data(),op.pts[0].x,op.pts[0].y,r,op.color);
    for(size_t i=1;i<op.pts.size();i++)
      lineBuf(g_baseInk.data(),op.pts[i-1].x,op.pts[i-1].y,op.pts[i].x,op.pts[i].y,r,op.color);
    if(!g_strokes.empty()) g_strokes.erase(g_strokes.begin());
  }
  g_ops.erase(g_ops.begin());
  if(g_pos>0) g_pos--;
}
static void pushOp(const Op& op){
  if((int)g_ops.size()>=timelineCap()) dropOldest();
  g_ops.push_back(op);
  g_pos=(int)g_ops.size();
  invalidateStatus();          // 时间轴变了 -> 底栏需重绘
}

// ---------------- 文本块快照 ----------------
static void snapBefore(Op& op){
  op.before.assign((size_t)op.rows*op.cols,0u);
  for(int r=0;r<op.rows;r++)
    for(int c=0;c<op.cols;c++){
      int rr=op.r0+r, cc=op.c0+c;
      if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols) op.before[(size_t)r*op.cols+c]=g_cells[(size_t)rr*g_cols+cc];
    }
}
static void snapAfter(Op& op){
  op.after.assign((size_t)op.rows*op.cols,0u);
  for(int r=0;r<op.rows;r++)
    for(int c=0;c<op.cols;c++){
      int rr=op.r0+r, cc=op.c0+c;
      if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols) op.after[(size_t)r*op.cols+c]=g_cells[(size_t)rr*g_cols+cc];
    }
}
static void putGlyph(int row,int col,uint32_t cp){
  Glyph gl=getGlyph(cp); int w=(gl.bits&&gl.w==2)?2:1;
  if(col+w>g_cols) w=1;
  clearGlyphAt(row,col);
  if(w==2) clearGlyphAt(row,col+1);
  g_cells[(size_t)row*g_cols+col]=cp;
  if(w==2 && col+1<g_cols) g_cells[(size_t)row*g_cols+col+1]=CONT;
}

// ---------------- 文字编辑 ----------------
static RECT placeChar(uint32_t cp){
  if(cp<32) return emptyRectPx();   // 控制字符不落格(回车等走光标逻辑)
  Glyph gl=getGlyph(cp);
  int w=(gl.bits&&gl.w==2)?2:1;
  if(g_cx+w>g_cols) return emptyRectPx();
  int row=g_cy;
  truncateFuture();
  int start=imax(0,g_cx-1);
  int end=imin(g_cols-1,g_cx+2);
  int len=end-start+1;
  Op op; op.type=OP_TEXT; op.r0=row; op.c0=start; op.rows=1; op.cols=len;
  snapBefore(op);
  int x0=g_cx,y0=g_cy;
  putGlyph(row,g_cx,cp);
  g_cx+=w; if(g_cx>g_cols-1) g_cx=g_cols-1;
  snapAfter(op);
  op.cbx0=x0; op.cby0=y0; op.cbx1=g_cx; op.cby1=g_cy;
  pushOp(op);
  renderTextRows(row,row);
  return rowsRectPx(row,row);
}
static RECT doBackspace(){
  if(g_cx<=0) return emptyRectPx();
  int row=g_cy;
  truncateFuture();
  int x0=g_cx,y0=g_cy;
  int c=g_cx;
  do { c--; } while(c>0 && g_cells[(size_t)row*g_cols+c]==CONT);
  g_cx=c;
  int start=imax(0,c-1), end=imin(g_cols-1,c+2), len=end-start+1;
  Op op; op.type=OP_TEXT; op.r0=row; op.c0=start; op.rows=1; op.cols=len;
  snapBefore(op);
  clearGlyphAt(row,c);
  snapAfter(op);
  op.cbx0=x0; op.cby0=y0; op.cbx1=g_cx; op.cby1=g_cy;
  pushOp(op);
  renderTextRows(row,row);
  return rowsRectPx(row,row);
}

// ---------------- 剪贴板 ----------------
static std::wstring regionText(int c0,int c1,int r0,int r1){
  std::wstring out;
  for(int r=r0;r<=r1;r++){
    std::wstring line;
    for(int c=c0;c<=c1;c++){
      uint32_t v=g_cells[(size_t)r*g_cols+c];
      if(v==CONT) continue;
      appendCP(line, v?v:L' ');
    }
    size_t e=line.find_last_not_of(L' ');
    if(e==std::wstring::npos) line.clear(); else line.erase(e+1);
    out+=line;
    if(r<r1) out+=L"\r\n";
  }
  return out;
}
static void setClipboardText(const std::wstring& out){
  if(!OpenClipboard(g_hwnd)) return;
  EmptyClipboard();
  size_t bytes=(out.size()+1)*sizeof(wchar_t);
  HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,bytes);
  if(g){ void* p=GlobalLock(g); if(p){ memcpy(p,out.c_str(),bytes); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT,g); } }
  CloseClipboard();
}
static void doCopy(){
  if(g_hasSel){
    int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
    setClipboardText(regionText(c0,c1,r0,r1));
  } else {
    int c=g_cx; uint32_t v=g_cells[(size_t)g_cy*g_cols+c];
    if(v==CONT){ c--; v=g_cells[(size_t)g_cy*g_cols+c]; }
    std::wstring out; appendCP(out, v?v:L' ');
    setClipboardText(out);
  }
}
static RECT doCut(){
  int c0,c1,r0,r1;
  if(g_hasSel){ selBounds(c0,c1,r0,r1); }
  else {
    int c=g_cx; uint32_t v=g_cells[(size_t)g_cy*g_cols+c];
    int w=1;
    if(v==CONT){ c--; v=g_cells[(size_t)g_cy*g_cols+c]; }
    if(v){ Glyph gl=getGlyph(v); w=(gl.w==2)?2:1; }
    c0=c; c1=c+w-1; r0=r1=g_cy;
    if(c1>g_cols-1) c1=g_cols-1;
    if(!v){ std::wstring o; appendCP(o,L' '); setClipboardText(o); return emptyRectPx(); } // 空格：只复制不删
  }
  setClipboardText(regionText(c0,c1,r0,r1));
  truncateFuture();
  Op op; op.type=OP_TEXT; op.r0=r0; op.c0=c0; op.rows=r1-r0+1; op.cols=c1-c0+1;
  snapBefore(op);
  for(int r=r0;r<=r1;r++) for(int c=c0;c<=c1;c++) g_cells[(size_t)r*g_cols+c]=0;
  snapAfter(op);
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=c0; op.cby1=r0;
  pushOp(op);
  g_cx=c0; g_cy=r0; g_hasSel=false;
  renderTextRows(r0,r1);
  g_dirty=true;
  return rowsRectPx(r0,r1);
}
static RECT doPaste(){
  if(!IsClipboardFormatAvailable(CF_UNICODETEXT)) return emptyRectPx();
  if(!OpenClipboard(g_hwnd)) return emptyRectPx();
  std::wstring t;
  HANDLE h=GetClipboardData(CF_UNICODETEXT);
  if(h){ const wchar_t* p=(const wchar_t*)GlobalLock(h); if(p){ t=p; GlobalUnlock(h);} }
  CloseClipboard();
  if(t.empty()) return emptyRectPx();
  std::vector<std::wstring> lines;
  for(size_t i=0;i<=t.size();){
    size_t j=t.find_first_of(L"\r\n",i);
    if(j==std::wstring::npos){ lines.push_back(t.substr(i)); break; }
    lines.push_back(t.substr(i,j-i));
    if(t[j]==L'\r'&&j+1<t.size()&&t[j+1]==L'\n') i=j+2; else i=j+1;
  }
  if(lines.empty()) return emptyRectPx();
  int rows=imin((int)lines.size(), g_H-g_cy);
  if(rows<=0) return emptyRectPx();
  std::vector<uint32_t> cps;
  int maxcols=0;
  for(int r=0;r<rows;r++){
    decodeCP(lines[r],cps);
    int wpx=0;
    for(size_t k=0;k<cps.size();k++){ if(cps[k]<32) continue; Glyph gl=getGlyph(cps[k]); wpx+=(gl.bits&&gl.w==2)?2:1; }
    if(wpx>maxcols) maxcols=wpx;
  }
  int c0=g_cx, r0=g_cy;
  int cols=imin(maxcols, g_cols-c0);
  if(cols<=0) return emptyRectPx();
  truncateFuture();
  Op op; op.type=OP_TEXT; op.r0=r0; op.c0=c0; op.rows=rows; op.cols=cols;
  snapBefore(op);
  std::vector<uint32_t> cps2;
  for(int r=0;r<rows;r++){
    decodeCP(lines[r],cps2);
    int c=c0;
    for(size_t k=0;k<cps2.size();k++){
      uint32_t cp=cps2[k];
      if(cp<32) continue;           // 控制字符不进画布
      Glyph gl=getGlyph(cp); int w=(gl.bits&&gl.w==2)?2:1;
      if(c+w>g_cols) break;
      putGlyph(r0+r,c,cp);
      c+=w;
    }
  }
  int caretC=imin(c0+cols, g_cols-1);   // 粘贴块右下角（右边界）
  int caretR=imin(r0+rows-1, g_H-1);
  snapAfter(op);
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=caretC; op.cby1=caretR;
  pushOp(op);
  g_cx=caretC; g_cy=caretR;
  renderTextRows(r0,r0+rows-1);
  g_dirty=true;
  return rowsRectPx(r0,r0+rows-1);
}

// ---------------- 画笔 ----------------
static void stampBuf(uint32_t* buf,int cx,int cy,int r,uint32_t col){
  for(int dy=-r;dy<=r;dy++){
    int y=cy+dy; if(y<0||y>=g_ph) continue;
    for(int dx=-r;dx<=r;dx++){
      if(dx*dx+dy*dy>r*r) continue;
      int x=cx+dx; if(x<0||x>=g_pw) continue;
      buf[(size_t)y*g_pw+x]=col;
    }
  }
}
static void lineBuf(uint32_t* buf,int x0,int y0,int x1,int y1,int r,uint32_t col){
  int dx=abs(x1-x0), sx=x0<x1?1:-1;
  int dy=-abs(y1-y0), sy=y0<y1?1:-1;
  int err=dx+dy;
  for(;;){
    stampBuf(buf,x0,y0,r,col);
    if(x0==x1&&y0==y1) break;
    int e2=2*err;
    if(e2>=dy){ err+=dy; x0+=sx; }
    if(e2<=dx){ err+=dx; y0+=sy; }
  }
}
static POINT snap8(int ax,int ay,int x,int y){
  int dx=x-ax, dy=y-ay;
  int adx=abs(dx), ady=abs(dy);
  int sx=(dx>0)-(dx<0), sy=(dy>0)-(dy<0);
  POINT e; const double T=0.41421356;
  if(ady <= adx*T){ e.x=ax+dx; e.y=ay; }
  else if(adx <= ady*T){ e.x=ax; e.y=ay+dy; }
  else { int m=(adx+ady)/2; e.x=ax+sx*m; e.y=ay+sy*m; }
  return e;
}
static void rebuildInk(){
  memcpy(g_ink.data(), g_baseInk.data(), (size_t)g_pw*g_ph*4);
  for(size_t s=0;s<g_strokes.size();s++){
    Stroke& st=g_strokes[s]; int r=st.size/2;
    if(st.pts.size()==1) stampBuf(g_ink.data(),st.pts[0].x,st.pts[0].y,r,st.color);
    for(size_t i=1;i<st.pts.size();i++)
      lineBuf(g_ink.data(),st.pts[i-1].x,st.pts[i-1].y,st.pts[i].x,st.pts[i].y,r,st.color);
  }
}

// ---------------- 历史时间轴 ----------------
static void rebuildState(int p){
  for(size_t i=0;i<g_cells.size();i++) g_cells[i]=g_baseCells[i];
  g_strokes.clear();
  for(int k=0;k<p;k++){
    Op& op=g_ops[k];
    if(op.type==OP_TEXT){
      for(int r=0;r<op.rows;r++)
        for(int c=0;c<op.cols;c++){
          int rr=op.r0+r, cc=op.c0+c;
          if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols) g_cells[(size_t)rr*g_cols+cc]=op.after[(size_t)r*op.cols+c];
        }
    } else {
      Stroke s; s.color=op.color; s.size=op.size; s.pts=op.pts; g_strokes.push_back(s);
    }
  }
  if(p>0){ g_cx=g_ops[p-1].cbx1; g_cy=g_ops[p-1].cby1; }
  rebuildInk(); renderText(); compose();
}
static void setPos(int p){
  if(p<0) p=0; if(p>(int)g_ops.size()) p=(int)g_ops.size();
  if(p==g_pos) return;
  g_pos=p; g_hasSel=false; g_selecting=false; rebuildState(g_pos); g_dirty=true;
  InvalidateRect(g_hwnd,nullptr,FALSE);
  overlayChanged();
}
static void doUndo(){ setPos(g_pos-1); }

// ---------------- 顶栏 / 底栏 / 选区 ----------------
static void putPx(int x,int y,uint32_t c){
  if(x<0||y<0||x>=g_cw||y>=g_ch||!g_memBits) return;
  g_memBits[(size_t)y*g_cw+x]=c;
}
static inline void invertPx(int x,int y){
  if(x<0||y<0||x>=g_cw||y>=g_ch) return;
  uint32_t v=g_memBits[(size_t)y*g_cw+x];
  g_memBits[(size_t)y*g_cw+x]=0xFF000000u|((~v)&0xFFFFFFu);
}
static void blitGlyph(uint32_t cp,int px,int py,uint32_t color){
  Glyph gl=getGlyph(cp); if(!gl.bits) return;
  for(int r=0;r<ROWH;r++){
    uint16_t row=(uint16_t)((gl.bits[r*2]<<8)|gl.bits[r*2+1]);
    if(!row) continue;
    int Y=py+r; if(Y<0||Y>=g_ch) continue;
    for(int k=0;k<16;k++){ if(row&(0x8000>>k)){ int X=px+k; if(X>=0&&X<g_cw) g_memBits[(size_t)Y*g_cw+X]=color; } }
  }
}
static void drawTopBar(){
  for(int y=0;y<g_TB;y++)
    for(int x=0;x<g_cw;x++) g_memBits[(size_t)y*g_cw+x]=0xFFF0F0F0u;
  for(int x=0;x<g_cw;x++) putPx(x,g_TB-1,0xFF808080u);
  {   // 帮助：普通小字，无色块
    HGDIOBJ oldF=SelectObject(g_memDC,g_uiFontSmall);
    int oldBk=SetBkMode(g_memDC,TRANSPARENT);
    COLORREF oldTx=SetTextColor(g_memDC,RGB(0x40,0x40,0x40));
    RECT hr={HELP_X,0,HELP_X+HELP_W,g_TB};
    DrawTextW(g_memDC,L"帮助",-1,&hr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SetTextColor(g_memDC,oldTx);
    SetBkMode(g_memDC,oldBk);
    SelectObject(g_memDC,oldF);
  }
  int cap=(g_cw-g_swatchX0)/g_swPitch;
  int n=(int)g_sw.size(); if(n>cap) n=cap;
  for(int i=0;i<n;i++){
    int x=g_swatchX0+i*g_swPitch, y=g_swatchY0;
    uint32_t col=g_sw[i].color; int kind=g_sw[i].kind;
    bool checker=(kind==3)||(kind==0&&((col>>24)==0));
    for(int k=0;k<g_swSz;k++){
      putPx(x+k,y,0xFF606060u); putPx(x+k,y+g_swSz-1,0xFF606060u);
      putPx(x,y+k,0xFF606060u); putPx(x+g_swSz-1,y+k,0xFF606060u);
    }
    for(int j=1;j<g_swSz-1;j++)
      for(int k=1;k<g_swSz-1;k++){
        uint32_t c;
        if(checker) c=((((k>>1)+(j>>1))&1)?0xFFFFFFFFu:0xFFB0B0B0u);
        else c=argb(col);
        putPx(x+k,y+j,c);
      }
  }
}
static void drawStatusBar(){
  int y0=g_TB+g_ph;
  for(int y=y0;y<g_ch;y++)
    for(int x=0;x<g_cw;x++) g_memBits[(size_t)y*g_cw+x]=0xFFF0F0F0u;
  for(int x=0;x<g_cw;x++) putPx(x,y0,0xFF808080u);
  uint32_t act=0xFF202020u, gray=0xFFB4B4B4u;
  int n=(int)g_ops.size(); int cap=g_cw/16; if(n>cap) n=cap;
  for(int i=0;i<n;i++){
    uint32_t col=(i<g_pos)?act:gray;
    uint32_t cp=(g_ops[i].type==OP_TEXT)?CP_WRITE:CP_DRAW;
    blitGlyph(cp, i*16, y0+1, col);
  }
}
static void drawSelection(){
  if(!g_selecting && !g_hasSel) return;
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  int x0=c0*COLW, y0=g_TB+r0*ROWH, x1=(c1+1)*COLW, y1=g_TB+(r1+1)*ROWH;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) invertPx(x,y);
}
static void drawCaret(){
  int row=g_cy, col=g_cx, w=1;
  uint32_t cp=g_cells[(size_t)row*g_cols+g_cx];
  if(cp==CONT){ col=g_cx-1; w=2; }
  else if(cp){ Glyph gl=getGlyph(cp); w=(gl.w==2)?2:1; }
  int bx=col*COLW, by=g_TB+row*ROWH;
  for(int y=0;y<ROWH;y++) for(int x=0;x<w*COLW;x++) invertPx(bx+x,by+y);
}
// 光标是否落在选区内：是则不再单独画（选区即"放大的光标"）
static bool caretInSel(){
  if(!g_selecting && !g_hasSel) return false;
  int c0=imin(g_selC0,g_selC1), c1=imax(g_selC0,g_selC1);
  int r0=imin(g_selR0,g_selR1), r1=imax(g_selR0,g_selR1);
  if(g_cy<r0||g_cy>r1) return false;
  int cx0=g_cx, cw=1;
  uint32_t cp=g_cells[(size_t)g_cy*g_cols+g_cx];
  if(cp==CONT){ cx0=g_cx-1; cw=2; }
  else if(cp){ Glyph gl=getGlyph(cp); cw=(gl.w==2)?2:1; }
  return cx0>=c0 && cx0+cw-1<=c1;
}

// ---------------- 颜色解析 ----------------
static bool parseColor(const wchar_t* s, uint32_t* out){
  while(*s==L' '||*s==L'#'||*s==L'\t') s++;
  if(wcschr(s,L',')){
    int r=0,g=0,b=0;
    if(swscanf(s,L"%d , %d , %d",&r,&g,&b)==3 &&
       r>=0&&r<256&&g>=0&&g<256&&b>=0&&b<256){ *out=RGB24(r,g,b); return true; }
    return false;
  }
  uint32_t v=0; int n=0;
  for(const wchar_t* p=s;*p;p++){
    if(*p==L' ') break;
    int d;
    if(*p>=L'0'&&*p<=L'9') d=*p-L'0';
    else if(*p>=L'a'&&*p<=L'f') d=*p-L'a'+10;
    else if(*p>=L'A'&&*p<=L'F') d=*p-L'A'+10;
    else return false;
    v=v*16+d; if(++n>6) return false;
  }
  if(n==6){ *out=v&0xFFFFFFu; return true; }
  return false;
}

// ---------------- 输入框 ----------------
static void refreshEdit(){
  if(g_hEdit){
    wchar_t b[16];
    if((g_brush>>24)==0) b[0]=0;
    else swprintf(b,16,L"%06X",(unsigned)(g_brush&0xFFFFFFu));
    SetWindowTextW(g_hEdit,b);
  }
}
static void refreshSize(){
  if(!g_hEditSize) return;
  wchar_t b[16]; swprintf(b,16,L"%d",g_size);
  SetWindowTextW(g_hEditSize,b);
}
static void commitColor(){
  wchar_t b[64]; GetWindowTextW(g_hEdit,b,64);
  uint32_t c; if(!parseColor(b,&c)) return;
  g_brush=argb(c);
  g_sw[0].color=g_brush;
  g_sw.push_back({argb(c),9});
  int cap=(g_cw-g_swatchX0)/g_swPitch;
  while((int)g_sw.size()>cap){
    bool removed=false;
    for(size_t i=0;i<g_sw.size();i++)
      if(g_sw[i].kind==9){ g_sw.erase(g_sw.begin()+i); removed=true; break; }
    if(!removed) break;
  }
  InvalidateRect(g_hwnd,nullptr,FALSE);
}
static void commitSize(){
  wchar_t b[32]; GetWindowTextW(g_hEditSize,b,32);
  int v=_wtoi(b); if(v<1) v=1; if(v>64) v=64;
  g_size=v; refreshSize();
}
static void clickSwatch(int x,int y){
  if(y<g_swatchY0||y>=g_swatchY0+g_swSz) return;
  if(x<g_swatchX0) return;
  int i=(x-g_swatchX0)/g_swPitch;
  int cap=(g_cw-g_swatchX0)/g_swPitch;
  if(i<0||i>=(int)g_sw.size()||i>=cap) return;
  g_brush=(g_sw[i].kind==3)?0u:argb(g_sw[i].color);
  g_sw[0].color=g_brush;
  refreshEdit();
  InvalidateRect(g_hwnd,nullptr,FALSE);
}
static LRESULT CALLBACK EditProc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_KEYDOWN && w==VK_RETURN){ if(h==g_hEditSize) commitSize(); else commitColor(); return 0; }
  if(m==WM_CHAR && w==13) return 0;
  if(m==WM_CHAR && w==27){ SetFocus(g_hwnd); return 0; }
  return CallWindowProc(g_editProc,h,m,w,l);
}

// ---------------- IME ----------------
static bool imeComposing(HWND hwnd){
  HIMC hImc=ImmGetContext(hwnd); if(!hImc) return false;
  LONG n=ImmGetCompositionStringW(hImc,GCS_COMPSTR,nullptr,0);
  ImmReleaseContext(hwnd,hImc);
  return n>0;
}
static void positionIme(HWND hwnd){
  HIMC hImc=ImmGetContext(hwnd); if(!hImc) return;
  int bx=g_cx*COLW, by=g_TB+g_cy*ROWH;
  COMPOSITIONFORM cf; memset(&cf,0,sizeof(cf));
  cf.dwStyle=CFS_POINT; cf.ptCurrentPos.x=bx; cf.ptCurrentPos.y=by;
  ImmSetCompositionWindow(hImc,&cf);
  CANDIDATEFORM can; memset(&can,0,sizeof(can));
  can.dwIndex=0; can.dwStyle=CFS_CANDIDATEPOS;
  can.ptCurrentPos.x=bx; can.ptCurrentPos.y=by+ROWH;
  ImmSetCandidateWindow(hImc,&can);
  ImmReleaseContext(hwnd,hImc);
}

// ---------------- PNG 导出 ----------------
static int GetEncoderClsid(const WCHAR* mime, CLSID* clsid){
  UINT num=0,size=0;
  if(GetImageEncodersSize(&num,&size)!=Ok||size==0) return -1;
  ImageCodecInfo* p=(ImageCodecInfo*)malloc(size);
  if(!p) return -1;
  GetImageEncoders(num,size,p);
  int ret=-1;
  for(UINT i=0;i<num;i++)
    if(wcscmp(p[i].MimeType,mime)==0){ *clsid=p[i].Clsid; ret=(int)i; break; }
  free(p);
  return ret;
}
static bool doSave(){
  wchar_t path[MAX_PATH]=L"texel.png";
  OPENFILENAMEW ofn;
  memset(&ofn,0,sizeof(ofn));
  ofn.lStructSize=sizeof(ofn);
  ofn.hwndOwner=g_hwnd;
  ofn.lpstrFilter=L"PNG (*.png)\0*.png\0All files\0*.*\0";
  ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
  ofn.lpstrDefExt=L"png";
  ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_EXPLORER;
  if(!GetSaveFileNameW(&ofn)) return false;
  Bitmap bmp(g_pw,g_ph,g_pw*4,PixelFormat32bppARGB,(BYTE*)g_fb.data());
  CLSID clsid;
  if(GetEncoderClsid(L"image/png",&clsid)<0) return false;
  if(bmp.Save(path,&clsid,nullptr)!=Ok) return false;
  g_dirty=false;
  return true;
}

// ---------------- 配置 ----------------
static void configPath(wchar_t* out,int n){
  wchar_t p[MAX_PATH];
  DWORD r=GetModuleFileNameW(nullptr,p,MAX_PATH);
  if(r==0||r>=MAX_PATH) wcscpy(p,L".");
  wchar_t* s=wcsrchr(p,L'\\');
  if(s && s!=p) *(s+1)=0; else wcscpy(p,L".");
  swprintf(out,n,L"%ls\\texel.ini",p);
}
static void loadConfig(){
  wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
  g_W=GetPrivateProfileIntW(L"cfg",L"W",50,p);
  g_H=GetPrivateProfileIntW(L"cfg",L"H",50,p);
  g_winX=GetPrivateProfileIntW(L"cfg",L"X",0,p);
  g_winY=GetPrivateProfileIntW(L"cfg",L"Y",0,p);
  g_hasPos=GetPrivateProfileIntW(L"cfg",L"Pos",0,p)!=0;
  g_skipNew=GetPrivateProfileIntW(L"cfg",L"SkipNew",0,p)!=0;
  g_noSavePrompt=GetPrivateProfileIntW(L"cfg",L"NoSavePrompt",0,p)!=0;
  wchar_t b[16]; uint32_t c;
  GetPrivateProfileStringW(L"cfg",L"Bg",L"FFFFFF",b,16,p); if(parseColor(b,&c)) g_bg=c;
  GetPrivateProfileStringW(L"cfg",L"Fg",L"000000",b,16,p); if(parseColor(b,&c)) g_fg=c;
  if(g_W<4) g_W=4; if(g_W>512) g_W=512;
  if(g_H<1) g_H=1; if(g_H>512) g_H=512;
}
static void saveConfig(){
  wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
  wchar_t b[16];
  swprintf(b,16,L"%d",g_W); WritePrivateProfileStringW(L"cfg",L"W",b,p);
  swprintf(b,16,L"%d",g_H); WritePrivateProfileStringW(L"cfg",L"H",b,p);
  swprintf(b,16,L"%06X",(unsigned)(g_bg&0xFFFFFF)); WritePrivateProfileStringW(L"cfg",L"Bg",b,p);
  swprintf(b,16,L"%06X",(unsigned)(g_fg&0xFFFFFF)); WritePrivateProfileStringW(L"cfg",L"Fg",b,p);
  WINDOWPLACEMENT wp; memset(&wp,0,sizeof(wp)); wp.length=sizeof(wp);
  if(g_hwnd && GetWindowPlacement(g_hwnd,&wp)){
    swprintf(b,16,L"%d",wp.rcNormalPosition.left); WritePrivateProfileStringW(L"cfg",L"X",b,p);
    swprintf(b,16,L"%d",wp.rcNormalPosition.top);  WritePrivateProfileStringW(L"cfg",L"Y",b,p);
    WritePrivateProfileStringW(L"cfg",L"Pos",L"1",p);
  }
}

static void writeIniDefaults(int W,int H,uint32_t bg,uint32_t fg){
  wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
  wchar_t b[16];
  swprintf(b,16,L"%d",W); WritePrivateProfileStringW(L"cfg",L"W",b,p);
  swprintf(b,16,L"%d",H); WritePrivateProfileStringW(L"cfg",L"H",b,p);
  swprintf(b,16,L"%06X",(unsigned)(bg&0xFFFFFF)); WritePrivateProfileStringW(L"cfg",L"Bg",b,p);
  swprintf(b,16,L"%06X",(unsigned)(fg&0xFFFFFF)); WritePrivateProfileStringW(L"cfg",L"Fg",b,p);
}

// ---------------- 新建对话框 ----------------
static const wchar_t* HELP_TEXT =
L"字画 texel —— 极简像素草稿本\r\n"
L"\r\n"
L"【画布】\r\n"
L"  尺寸 = 窗口，锁定不可缩放；单位『字』= 16×16 像素。\r\n"
L"  背景色 = 画布底色，前景色 = 文字颜色（都填 #RRGGBB），创建后不可改。\r\n"
L"\r\n"
L"【图层】文字层在下，笔迹层在上（笔迹盖住文字）。\r\n"
L"\r\n"
L"【鼠标】\r\n"
L"  左键单击：定位文字光标；点底栏 = 切换历史。\r\n"
L"  左键长按/拖动：矩形选中，选中区反色显示。\r\n"
L"  右键拖动：画笔；按住 Shift：八向直线。\r\n"
L"  滚轮：调整画笔粗细。\r\n"
L"\r\n"
L"【键盘】\r\n"
L"  直接打字：覆盖当前格，光标右移。\r\n"
L"  方向键：移动光标；Home / End：行首 / 行尾。\r\n"
L"  Enter：下一行行首；Backspace：删左边一个字。\r\n"
L"  Ctrl+A：全选；Ctrl+C：复制；Ctrl+X：剪切；Ctrl+V：粘贴（算一次『写』）。\r\n"
L"  Ctrl+Z：回撤；Ctrl+Y：重做。\r\n"
L"  Ctrl+S：保存 PNG；Ctrl+N：新建；Ctrl+L：清空笔迹。\r\n"
L"\r\n"
L"【顶栏】\r\n"
L"  粗：画笔粗细 (1-64)；颜色：当前画笔色 (#RRGGBB)。\r\n"
L"  色块：当前 / 前景 / 背景 / 透明 / 经典16色 / 历史。\r\n"
L"  改颜色后追加历史色块，栏满挤掉最旧（前、背景与常用色固定）。\r\n"
L"\r\n"
L"【底栏】历史时间轴，从左向右生长：写 = 文字，画 = 笔迹；\r\n"
L"  深色 = 已应用，灰色 = 已回撤；点击切换，满了挤掉最旧。\r\n"
L"\r\n"
L"【配置 texel.ini（可手动编辑）】\r\n"
L"  SkipNew=1       启动不弹本窗口，直接用上次参数新建\r\n"
L"  NoSavePrompt=1  退出不提示保存，直接关闭\r\n"
L"  对话框里的『设为默认值』= 保存当前参数 + SkipNew=1\r\n"
L"\r\n"
L"  退出时若未保存，会提示保存。\r\n";

static void showHelp(){
  MessageBoxW(g_hwnd,HELP_TEXT,L"字画 · 帮助",MB_OK|MB_ICONINFORMATION);
}

static INT_PTR CALLBACK NewDlgProc(HWND h,UINT m,WPARAM w,LPARAM){
  switch(m){
    case WM_INITDIALOG:{
      wchar_t b[32];
      swprintf(b,32,L"%d",g_W); SetDlgItemTextW(h,IDC_W,b);
      swprintf(b,32,L"%d",g_H); SetDlgItemTextW(h,IDC_H,b);
      swprintf(b,32,L"%06X",(unsigned)(g_bg&0xFFFFFF)); SetDlgItemTextW(h,IDC_BG,b);
      swprintf(b,32,L"%06X",(unsigned)(g_fg&0xFFFFFF)); SetDlgItemTextW(h,IDC_FG,b);
      SetDlgItemTextW(h,IDC_HELPTEXT,HELP_TEXT);
      return TRUE;
    }
    case WM_COMMAND:
      if(LOWORD(w)==IDC_SETDEF){
        wchar_t b[32]; uint32_t bg,fg;
        GetDlgItemTextW(h,IDC_W,b,32); int W=_wtoi(b);
        GetDlgItemTextW(h,IDC_H,b,32); int H=_wtoi(b);
        GetDlgItemTextW(h,IDC_BG,b,32); if(!parseColor(b,&bg)) bg=g_bg;
        GetDlgItemTextW(h,IDC_FG,b,32); if(!parseColor(b,&fg)) fg=g_fg;
        if(W<4)W=4; if(W>512)W=512; if(H<1)H=1; if(H>512)H=512;
        writeIniDefaults(W,H,bg,fg);
        {
          wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
          WritePrivateProfileStringW(L"cfg",L"SkipNew",L"1",p);   // 不再显示本窗口
        }
        MessageBoxW(h,L"已把当前参数设为默认值，并关闭“新建画布”提示（下次启动生效）。",L"字画",MB_OK|MB_ICONINFORMATION);
        return TRUE;
      }
      if(LOWORD(w)==IDOK){
        wchar_t b[32]; uint32_t c;
        GetDlgItemTextW(h,IDC_W,b,32); int W=_wtoi(b);
        GetDlgItemTextW(h,IDC_H,b,32); int H=_wtoi(b);
        GetDlgItemTextW(h,IDC_BG,b,32); if(!parseColor(b,&c)) c=g_bg;
        GetDlgItemTextW(h,IDC_FG,b,32); uint32_t f; if(!parseColor(b,&f)) f=g_fg;
        if(W<4) W=4; if(W>512) W=512;
        if(H<1) H=1; if(H>512) H=512;
        g_W=W; g_H=H; g_bg=c; g_fg=f;
        EndDialog(h,1); return TRUE;
      }
      if(LOWORD(w)==IDCANCEL){ EndDialog(h,0); return TRUE; }
      break;
  }
  return FALSE;
}
static bool askParams(){
  return DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_NEW),g_hwnd,NewDlgProc,0)==1;
}

// 退出保存提示：0=取消(不退出) 1=是 2=否 3=否且不再提示
static INT_PTR CALLBACK SaveDlgProc(HWND h,UINT m,WPARAM w,LPARAM){
  if(m==WM_COMMAND){
    switch(LOWORD(w)){
      case IDC_SAVE_YES:   EndDialog(h,1); return TRUE;
      case IDC_SAVE_NO:    EndDialog(h,2); return TRUE;
      case IDC_SAVE_NEVER: EndDialog(h,3); return TRUE;
      case IDCANCEL:       EndDialog(h,0); return TRUE;
    }
  }
  return FALSE;
}

// ---------------- 画布 ----------------
static void createDIB(){
  if(g_memBmp){ SelectObject(g_memDC,g_memBmpOld); DeleteObject(g_memBmp); g_memBmp=nullptr; }
  BITMAPINFO bi; memset(&bi,0,sizeof(bi));
  bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth=g_cw;
  bi.bmiHeader.biHeight=-g_ch;
  bi.bmiHeader.biPlanes=1;
  bi.bmiHeader.biBitCount=32;
  bi.bmiHeader.biCompression=BI_RGB;
  g_memBmp=CreateDIBSection(g_memDC,&bi,DIB_RGB_COLORS,(void**)&g_memBits,nullptr,0);
  g_memBmpOld=(HBITMAP)SelectObject(g_memDC,g_memBmp);
}
static void setupCanvas(int W,int H,uint32_t bg,uint32_t fg){
  g_W=W; g_H=H; g_bg=bg; g_fg=fg;
  g_cols=W*2;
  g_pw=W*16; g_ph=H*16; g_cw=g_pw; g_ch=g_TB+g_ph+g_SB;
  g_cells.assign((size_t)g_H*g_cols,0u);
  g_ink.assign((size_t)g_pw*g_ph,0u);
  g_text.assign((size_t)g_pw*g_ph,0u);
  g_fb.assign((size_t)g_pw*g_ph,0u);
  g_strokes.clear(); g_ops.clear(); g_pos=0; baseReset();
  g_cx=0; g_cy=0; g_size=1; g_brush=argb(fg); g_dirty=false;
  g_hasSel=false; g_selecting=false; g_lbtnDown=false;
  g_sw.clear();
  g_sw.push_back({g_brush,0});
  g_sw.push_back({argb(fg),1});
  g_sw.push_back({argb(bg),2});
  g_sw.push_back({0u,3});
  for(int i=0;i<16;i++) g_sw.push_back({argb(EGA16[i]),4});
  createDIB();
  RECT rc={0,0,g_cw,g_ch};
  AdjustWindowRectEx(&rc, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, FALSE, 0);
  SetWindowPos(g_hwnd,nullptr,0,0,rc.right-rc.left,rc.bottom-rc.top,SWP_NOMOVE|SWP_NOZORDER);
  refreshEdit(); refreshSize();
  renderText(); compose();
  InvalidateRect(g_hwnd,nullptr,FALSE);
}
static void placeWindow(){
  RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
  RECT wr; GetWindowRect(g_hwnd,&wr);
  int ww=wr.right-wr.left, wh=wr.bottom-wr.top, x,y;
  bool restore = g_hasPos
      && g_winX > wa.left-ww+60 && g_winX < wa.right-60
      && g_winY > wa.top-10    && g_winY < wa.bottom-60;
  if(restore){ x=g_winX; y=g_winY; }
  else { x=wa.left+((wa.right-wa.left)-ww)/2; y=wa.top+((wa.bottom-wa.top)-wh)/2; }
  SetWindowPos(g_hwnd,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
    case WM_ERASEBKGND: return 1;

    case WM_PAINT:{
      PAINTSTRUCT ps;
      HDC dc=BeginPaint(hwnd,&ps);
      RECT R=ps.rcPaint;
      drawTopBar();                     // 顶栏/底栏很矮，整体重画即可
      drawStatusBar();
      int cy0=R.top-g_TB; if(cy0<0)cy0=0;
      int cy1=R.bottom-1-g_TB; if(cy1>g_ph-1)cy1=g_ph-1;
      for(int y=cy0;y<=cy1;y++)         // 只拷画布受影响的行
        memcpy(g_memBits+(size_t)(y+g_TB)*g_cw, g_fb.data()+(size_t)y*g_pw, (size_t)g_pw*4);
      drawSelection();
      if(!caretInSel()) drawCaret();
      g_prevOverlay=overlayRectPx();
      int w=R.right-R.left, h=R.bottom-R.top;
      if(w>0&&h>0) BitBlt(dc,R.left,R.top,w,h,g_memDC,R.left,R.top,SRCCOPY);
      EndPaint(hwnd,&ps);
      return 0;
    }

    case WM_LBUTTONDOWN:{
      SetFocus(hwnd);
      int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
      if(y<g_TB){
        if(x>=HELP_X && x<HELP_X+HELP_W && y>=HELP_Y && y<HELP_Y+HELP_H){ showHelp(); return 0; }
        clickSwatch(x,y); return 0;
      }
      if(y>=g_TB+g_ph){ int i=x/16; if(i>=0&&i<(int)g_ops.size()) setPos(i+1); return 0; }
      int cy=y-g_TB;
      int col=x/COLW, row=cy/ROWH;
      col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
      if(g_cells[(size_t)row*g_cols+col]==CONT) col--;
      g_cx=col; g_cy=row;           // 按下即定位光标（点击/长按统一）
      g_lbtnDown=true; g_selecting=false; g_hasSel=false;
      g_pressPt.x=x; g_pressPt.y=y;
      g_selAnchorC=col; g_selAnchorR=row;
      g_selC0=g_selC1=col; g_selR0=g_selR1=row;
      SetTimer(hwnd,2,400,nullptr);
      overlayChanged();
      return 0;
    }
    case WM_MOUSEMOVE:{
      int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
      if(g_drawing){
        int cy=y-g_TB;
        Stroke& st=g_strokes.back();
        int sx=st.pts.back().x, sy=st.pts.back().y;
        if(GetKeyState(VK_SHIFT)&0x8000){
          int ax=st.pts[0].x, ay=st.pts[0].y;
          POINT e=snap8(ax,ay,x,cy);
          st.pts.clear(); st.pts.push_back({ax,ay}); st.pts.push_back(e);
          rebuildInk(); redrawAllCanvas();
        } else {
          lineBuf(g_ink.data(),sx,sy,x,cy,g_size/2,g_brush);
          st.pts.push_back({x,cy});
          commitRect(brushRectPx(sx,sy,x,cy,g_size/2));
        }
        g_last.x=x; g_last.y=cy;
        return 0;
      }
      if(g_lbtnDown){
        if(!g_selecting){
          int dx=x-g_pressPt.x, dy=y-g_pressPt.y;
          if(dx*dx+dy*dy>64){ g_selecting=true; KillTimer(hwnd,2); }
        }
        if(g_selecting){
          int col=x/COLW, row=(y-g_TB)/ROWH;
          col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
          g_selC1=col; g_selR1=row;
          overlayChanged();
        }
        return 0;
      }
      return 0;
    }
    case WM_LBUTTONUP:{
      KillTimer(hwnd,2);
      if(g_lbtnDown){
        if(g_selecting){ g_selecting=false; g_hasSel=true; }
        // 单击：光标已在按下时定位，无需再处理
        g_lbtnDown=false;
        overlayChanged();
      }
      return 0;
    }
    case WM_RBUTTONDOWN:{
      int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
      if(y<g_TB||y>=g_TB+g_ph) return 0;
      g_hasSel=false;
      int cy=y-g_TB;
      truncateFuture();
      g_drawing=true; SetCapture(hwnd);
      g_last.x=x; g_last.y=cy;
      Stroke st; st.color=g_brush; st.size=g_size; st.pts.push_back({x,cy});
      g_strokes.push_back(st);
      Op op; op.type=OP_STROKE; op.color=g_brush; op.size=g_size;
      op.pts.push_back({x,cy}); op.cbx1=g_cx; op.cby1=g_cy;
      pushOp(op);
      stampBuf(g_ink.data(),x,cy,g_size/2,g_brush);
      g_dirty=true;
      overlayChanged();
      commitRect(brushRectPx(x,cy,x,cy,g_size/2));
      return 0;
    }
    case WM_RBUTTONUP:{
      if(g_drawing){
        g_drawing=false; ReleaseCapture();
        if(!g_ops.empty()) g_ops.back().pts=g_strokes.back().pts;
      }
      return 0;
    }
    case WM_CONTEXTMENU: return 0;

    case WM_MOUSEWHEEL:{
      int dz=GET_WHEEL_DELTA_WPARAM(wp);
      if(dz>0) g_size++; else if(dz<0) g_size--;
      if(g_size<1) g_size=1; if(g_size>64) g_size=64;
      refreshSize();
      return 0;
    }

    case WM_TIMER:{
      if(wp==2 && g_lbtnDown && !g_selecting){
        g_selecting=true; KillTimer(hwnd,2);
        overlayChanged();
      }
      return 0;
    }

    case WM_KEYDOWN:{
      if(imeComposing(hwnd)) return DefWindowProc(hwnd,msg,wp,lp);
      if(GetKeyState(VK_CONTROL)&0x8000){
        if(wp=='Z'){ doUndo(); return 0; }
        if(wp=='Y'){ setPos(g_pos+1); return 0; }
        if(wp=='S'){ doSave(); return 0; }
        if(wp=='N'){ if(g_skipNew || askParams()) setupCanvas(g_W,g_H,g_bg,g_fg); return 0; }
        if(wp=='C'){ doCopy(); return 0; }
        if(wp=='X'){ RECT r=doCut(); commitRect(r); overlayChanged(); return 0; }
        if(wp=='A'){ g_hasSel=true; g_selecting=false; g_selC0=0; g_selC1=g_cols-1; g_selR0=0; g_selR1=g_H-1; overlayChanged(); return 0; }
        if(wp=='V'){ RECT r=doPaste(); commitRect(r); overlayChanged(); return 0; }
        if(wp=='L'){ truncateFuture(); g_ops.clear(); g_pos=0; baseReset(); g_strokes.clear(); g_hasSel=false; rebuildInk(); renderText(); compose(); g_dirty=true; redrawAllCanvas(); overlayChanged(); invalidateStatus(); return 0; }
      }
      int row=g_cy; bool moved=false;
      switch(wp){
        case VK_LEFT:
          if(g_cx>0){ g_cx--; while(g_cx>0&&g_cells[(size_t)row*g_cols+g_cx]==CONT) g_cx--; moved=true; } break;
        case VK_RIGHT:
          if(g_cx<g_cols-1){ g_cx++; while(g_cx<g_cols-1&&g_cells[(size_t)row*g_cols+g_cx]==CONT) g_cx++; moved=true; } break;
        case VK_UP:    if(g_cy>0){g_cy--;moved=true;} break;
        case VK_DOWN:  if(g_cy<g_H-1){g_cy++;moved=true;} break;
        case VK_HOME:  g_cx=0; moved=true; break;
        case VK_END:
          g_cx=g_cols-1; while(g_cx>0&&g_cells[(size_t)row*g_cols+g_cx]==CONT) g_cx--; moved=true; break;
        case VK_RETURN: if(g_cy<g_H-1) g_cy++; g_cx=0; moved=true; break;
        case VK_PRIOR: case VK_NEXT: break;
        default: return DefWindowProc(hwnd,msg,wp,lp);
      }
      if(moved) overlayChanged();
      return 0;
    }
    case WM_CHAR:{
      wchar_t ch=(wchar_t)wp;
      uint32_t cp;
      if((uint32_t)ch>=0xD800 && (uint32_t)ch<=0xDBFF){ g_pendingHigh=ch; return 0; }   // 高代理，等低位
      if((uint32_t)ch>=0xDC00 && (uint32_t)ch<=0xDFFF){
        if(!g_pendingHigh) return 0;
        cp=0x10000u+(((uint32_t)g_pendingHigh-0xD800u)<<10)+((uint32_t)ch-0xDC00u);
        g_pendingHigh=0;
      } else { g_pendingHigh=0; cp=(uint32_t)ch; }
      if(cp>=32 && cp!=127){ RECT r=placeChar(cp); if(!rectEmpty(r)){ g_dirty=true; commitRect(r); overlayChanged(); } }
      else if(cp==8){ RECT r=doBackspace(); if(!rectEmpty(r)){ g_dirty=true; commitRect(r); overlayChanged(); } }
      return 0;
    }

    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
      positionIme(hwnd);
      break;

    case WM_CLOSE:
      if(g_dirty && !g_noSavePrompt){
        int r=(int)DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_SAVE),hwnd,SaveDlgProc,0);
        if(r==0) return 0;
        if(r==1){ if(!doSave()) return 0; }
        else if(r==3){
          wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
          WritePrivateProfileStringW(L"cfg",L"NoSavePrompt",L"1",p);
          g_noSavePrompt=true;
        }
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProc(hwnd,msg,wp,lp);
}

// ---------------- 入口 ----------------
int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int){
  g_hInst=hInst;
  SetProcessDPIAware();
  GdiplusStartupInput gi; ULONG_PTR tok=0;
  GdiplusStartup(&tok,&gi,nullptr);

  loadConfig();
  if(!g_skipNew && !askParams()) return 0;

  WNDCLASSEXW wc; memset(&wc,0,sizeof(wc));
  wc.cbSize=sizeof(wc);
  wc.style=CS_HREDRAW|CS_VREDRAW;
  wc.lpfnWndProc=WndProc;
  wc.hInstance=hInst;
  wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
  wc.hIcon=LoadIconW(hInst,MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm=LoadIconW(hInst,MAKEINTRESOURCEW(IDI_APP));
  wc.lpszClassName=L"TexelClass";
  RegisterClassExW(&wc);

  g_cols=g_W*2;
  g_pw=g_W*16; g_ph=g_H*16; g_cw=g_pw; g_ch=g_TB+g_ph+g_SB;

  RECT rc={0,0,g_cw,g_ch};
  AdjustWindowRectEx(&rc, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN, FALSE, 0);
  g_hwnd=CreateWindowExW(0,L"TexelClass",L"字画",
      WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,
      CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,
      nullptr,nullptr,hInst,nullptr);
  if(!g_hwnd) return 0;

  HRSRC hr=FindResourceW(hInst,L"UNIFONT",RT_RCDATA);
  if(hr){
    HGLOBAL hg=LoadResource(hInst,hr);
    g_font=(const uint8_t*)LockResource(hg);
    g_fontN=SizeofResource(hInst,hr)/37;
  }

  HDC dc=GetDC(g_hwnd);
  g_memDC=CreateCompatibleDC(dc);
  ReleaseDC(g_hwnd,dc);

  g_uiFont=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,FIXED_PITCH,L"Consolas");
  g_uiFontSmall=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"MS Shell Dlg");

  // 输入框宽度按等宽字体实测：色框留 8 字余量(6位hex+2，避免输入时滚动/看不全)，粗细框留 3 位
  int sizeX,sizeW,colorX,colorW;
  { HDC mdc=CreateCompatibleDC(nullptr); HGDIOBJ of=SelectObject(mdc,g_uiFont);
    SIZE sz; GetTextExtentPoint32W(mdc,L"000000",6,&sz);
    int cw=(sz.cx+5)/6, edge=GetSystemMetrics(SM_CXEDGE);   // cw=单字符宽
    sizeW =cw*3+edge*2+6;
    colorW=cw*8+edge*2+6;
    SelectObject(mdc,of); DeleteDC(mdc); }
  sizeX=40; colorX=sizeX+sizeW+6; g_swatchX0=colorX+colorW+8;

  g_hEditSize=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
      WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_NUMBER,
      sizeX,(g_TB-16)/2,sizeW,16,g_hwnd,(HMENU)IDC_EDITSIZE,hInst,nullptr);
  g_editProc=(WNDPROC)SetWindowLongPtrW(g_hEditSize,GWLP_WNDPROC,(LONG_PTR)EditProc);
  SendMessageW(g_hEditSize,WM_SETFONT,(WPARAM)g_uiFont,TRUE);

  g_hEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
      WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
      colorX,(g_TB-16)/2,colorW,16,g_hwnd,(HMENU)IDC_EDITRGB,hInst,nullptr);
  SetWindowLongPtrW(g_hEdit,GWLP_WNDPROC,(LONG_PTR)EditProc);
  SendMessageW(g_hEdit,WM_SETFONT,(WPARAM)g_uiFont,TRUE);

  setupCanvas(g_W,g_H,g_bg,g_fg);
  placeWindow();

  ShowWindow(g_hwnd,SW_SHOW);
  UpdateWindow(g_hwnd);
  SetFocus(g_hwnd);

  MSG msg;
  while(GetMessageW(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }

  saveConfig();
  GdiplusShutdown(tok);
  return 0;
}
