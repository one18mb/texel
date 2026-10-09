// 字画 texel - 极简像素草稿本
// 顶栏: 粗细/颜色/色块   画布: 笔迹层(下)+文字层(上)   底栏: 历史时间轴
// 左键点=定位文字光标, 左键长按/拖动=矩形选中(反色); 右键=画笔(Shift=八向直线)
// Ctrl+C/V 复制粘贴(粘贴算一次"写"); Ctrl+Z/Y 回撤/重做; Ctrl+S 保存
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
#include <shellapi.h>
#include <ole2.h>
#include <cmath>
#include <cwctype>
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
static uint32_t g_bg = 0x00FFFFFF, g_fg = 0x000000; // 背景默认透明(RGB 白, alpha 0)

static std::vector<uint32_t> g_cells;
static std::vector<uint32_t> g_cellColor;   // 每个字形格的颜色(ARGB)
static std::vector<uint32_t> g_ink;
static std::vector<uint32_t> g_text;
static std::vector<uint32_t> g_fb;

static uint32_t g_brush = 0xFF000000;
static int      g_size  = 1;
static int      g_maxSize = 64;      // 画笔最大粗细 = 画布对角线
static DWORD    g_wheelT = 0;        // 上次滚轮时间
static double   g_wheelEma = 0.0;    // 平滑后的间隔(EMA)，用于测速
static double   g_wheelGain = 1024.0;// 滚轮加速封顶(ini: WheelGain)
static int      g_cx = 0, g_cy = 0;
static int      g_TB = 24;
static int      g_SB = 17;
static const int STATUS_COORD_W = 64;   // 底栏左下角坐标 y,x + 分隔符 的像素宽度(7字符*8 + '|')

static const uint8_t* g_font = nullptr;
static uint32_t       g_fontN = 0;

struct Stroke { uint32_t color; int size; std::vector<POINT> pts; };
static std::vector<Stroke> g_strokes;

// 历史操作：文字用【矩形块】存 before/after，笔迹存点
enum { OP_STROKE = 0, OP_TEXT = 1, OP_REGION = 2 };
struct Op {
  int type=OP_STROKE;
  int r0=0,c0=0,rows=0,cols=0;
  std::vector<uint32_t> before, after;      // 码点
  std::vector<uint32_t> beforeC, afterC;    // 颜色(与 before/after 平行)
  std::vector<uint32_t> inkB, inkA;         // 墨迹区域(像素)
  int ix=0,iy=0,iw=0,ih=0;                  // 墨迹区域的像素矩形
  int cbx0=0,cby0=0,cbx1=0,cby1=0;
  uint32_t color=0; int size=0;
  std::vector<POINT> pts;
};
static std::vector<Op> g_ops;
static int             g_pos = 0;
static std::vector<uint32_t> g_baseCells;   // 基础文字(仍是格子)
static std::vector<uint32_t> g_baseCellColor;// 基础文字的颜色
static std::vector<uint32_t> g_baseInk;     // 基础笔迹(已栅格化位图)
static UINT g_cfCells=0;                    // 私有剪贴板格式: 带色文字块
static UINT g_cfPNG=0;                      // 剪贴板 "PNG" 格式
static UINT g_cfDragImageBits=0;            // "DragImageBits"（浏览器/Shell 拖图位图）
static UINT g_cfFileContents=0;             // "FileContents"（Chromium 虚拟文件流）
static void writeInkTo(uint32_t* dst,const Op& op);   // 前置声明
static int  GetEncoderClsid(const WCHAR* mime,CLSID* clsid);
static void setupCanvas(int W,int H,uint32_t bg,uint32_t fg);
static void openTexelZipFile(const wchar_t* path);
static RECT pasteTexelZipFile(const wchar_t* path);
static std::vector<std::wstring> hdropFiles(HANDLE h);
static bool endsWithZW(const std::wstring& s,const wchar_t* suf);
static RECT pasteDroppedFile(const wchar_t* p);

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
static int    g_cw = 0, g_ch = 0;   // 客户区(呈现)尺寸
static int    g_ox = 0;              // 画布列左偏移(水平居中)
static int    g_visW = 0;            // 画布可见宽度(≤窗口宽，防越界)
static int    g_canvasY = 0, g_visH = 0;  // 画布顶(纵向居中)与可见高度
static bool   g_setup = false;    // 设置模式(无文件启动时先设置，未新建)
static int    g_editSizeX=40, g_editSizeW=30, g_colorX=74, g_colorW=48;
static WNDPROC g_editProc = nullptr;
static HFONT  g_uiFont = nullptr;
static HFONT  g_uiFontSmall = nullptr;
static bool   g_drawing = false;
static bool   g_dirty   = false;
static int    g_winX = 0, g_winY = 0;
static bool   g_hasPos = false;
static POINT  g_last = {0,0};
static int    g_swatchX0 = 130, g_swatchY0 = 5, g_swPitch = 16, g_swSz = 14;
static int    g_minWinW = 0, g_minWinH = 0;   // 窗口最小客户区尺寸（宽=放得下全部默认色块，高=放得下设置控件）

// 选中
static bool   g_lbtnDown=false, g_selecting=false;
static int    g_selAnchorC=0, g_selAnchorR=0;   // 长按锚点
static int    g_selDragC=0, g_selDragR=0;       // 长按活动端
static std::vector<uint8_t> g_selMask;          // 选区布尔表(每半格)
static bool   g_ctrlSel=false, g_ctrlDesel=false;  // Ctrl 左键选中 / 右键取消
static POINT  g_pressPt = {0,0};
static wchar_t g_pendingHigh = 0;   // UTF-16 高代理暂存
static bool   g_noSavePrompt = false; // ini: NoSavePrompt
static const int HELP_X=4, HELP_Y=4, HELP_W=32, HELP_H=16;

static inline uint32_t RGB24(int r,int g,int b){ return (uint32_t)((r<<16)|(g<<8)|b); }
static inline uint32_t argb(uint32_t c){ return 0xFF000000u | (c & 0xFFFFFFu); }
static inline uint32_t textColor(){ return g_brush; }   // 文字用当前色(透明则存透明=不可见)
static inline uint32_t over(uint32_t d,uint32_t s){   // src 覆盖 dst (含 alpha 混合)
  uint32_t a=s>>24; if(a==0) return d; if(a==255) return 0xFF000000u|(s&0xFFFFFFu);
  uint32_t ia=255-a;
  uint32_t r=(((s>>16)&0xFF)*a+((d>>16)&0xFF)*ia)/255;
  uint32_t g=(((s>>8)&0xFF)*a+((d>>8)&0xFF)*ia)/255;
  uint32_t b=((s&0xFF)*a+(d&0xFF)*ia)/255;
  return 0xFF000000u|(r<<16)|(g<<8)|b;
}
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

// ---- 选区布尔表（每半格；全角左右半格绑定，文字/图像统一） ----
static void selClear(){ g_selMask.assign((size_t)g_cols*g_H,0); }
static bool selGet(int c,int r){
  if(c<0||c>=g_cols||r<0||r>=g_H) return false;
  return g_selMask[(size_t)r*g_cols+c]!=0;
}
static void selSet(int c,int r,bool v){
  if(c<0||c>=g_cols||r<0||r>=g_H) return;
  if(g_cells[(size_t)r*g_cols+c]==CONT) c--;
  if(c<0||c>=g_cols) return;
  g_selMask[(size_t)r*g_cols+c]=v?1:0;
  uint32_t cp=g_cells[(size_t)r*g_cols+c];
  if(cp&&cp!=CONT){ Glyph gl=getGlyph(cp); if(gl.w==2&&c+1<g_cols) g_selMask[(size_t)r*g_cols+c+1]=v?1:0; }
}
static void selCaret(){ selClear(); selSet(g_cx,g_cy,true); }
// 矩形选区（惰性外扩：边界全角半格补一步，不递归）
static void selRect(int c0,int c1,int r0,int r1){
  selClear();
  if(c0>c1){ int t=c0; c0=c1; c1=t; }
  if(r0>r1){ int t=r0; r0=r1; r1=t; }
  if(c0<0)c0=0; if(c1>g_cols-1)c1=g_cols-1;
  if(r0<0)r0=0; if(r1>g_H-1)r1=g_H-1;
  for(int r=r0;r<=r1;r++) for(int c=c0;c<=c1;c++){
    uint32_t cp=g_cells[(size_t)r*g_cols+c];
    if(cp==CONT) continue;                     // 右半格由左格处理
    if(cp==0){ g_selMask[(size_t)r*g_cols+c]=1; continue; }
    Glyph gl=getGlyph(cp);
    if(gl.w==2){                                // 全角：左右半格都在矩形内才选，否则整字不选
      if(c+1<=c1 && c+1<g_cols){ g_selMask[(size_t)r*g_cols+c]=1; g_selMask[(size_t)r*g_cols+c+1]=1; }
    } else g_selMask[(size_t)r*g_cols+c]=1;
  }
}
static bool selAny(){
  for(uint8_t v:g_selMask) if(v) return true;
  return false;
}
// 格网连线（Bresenham），逐格 selSet
static void selLine(int c0,int r0,int c1,int r1,bool v){
  int dc=abs(c1-c0), dr=abs(r1-r0);
  int sc=c0<c1?1:-1, sr=r0<r1?1:-1;
  int err=dc-dr;
  for(;;){
    selSet(c0,r0,v);
    if(c0==c1 && r0==r1) break;
    int e2=2*err;
    if(e2>-dr){ err-=dr; c0+=sc; }
    if(e2<dc){ err+=dc; r0+=sr; }
  }
}
static void selBounds(int& c0,int& c1,int& r0,int& r1){
  c0=g_cols; c1=-1; r0=g_H; r1=-1;
  for(int r=0;r<g_H;r++) for(int c=0;c<g_cols;c++){
    if(g_selMask[(size_t)r*g_cols+c]){ if(c<c0)c0=c; if(c>c1)c1=c; if(r<r0)r0=r; if(r>r1)r1=r; }
  }
  if(c1<c0){ c0=c1=r0=r1=0; }
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
  uint32_t bgg=0u;                              // 文字层只留字形(透明底)，合成时置于墨迹之上
  for(int y=r0;y<=r1;y++){
    int by=y*ROWH;
    for(int r=0;r<ROWH;r++){ int py=by+r; if(py<0||py>=g_ph) continue;
      uint32_t* p=g_text.data()+(size_t)py*g_pw; for(int x=0;x<g_pw;x++) p[x]=bgg; }
    for(int c=0;c<g_cols;c++){
      uint32_t cp=g_cells[(size_t)y*g_cols+c];
      if(!cp||cp==CONT) continue;
      Glyph gl=getGlyph(cp); if(!gl.bits) continue;
      uint32_t fgc=g_cellColor[(size_t)y*g_cols+c];
      if((fgc>>24)==0) continue;              // 透明字：字在但无墨
      int bx=c*COLW, maxc=(gl.w==2)?16:8;
      for(int r=0;r<ROWH;r++){
        uint16_t row=(uint16_t)((gl.bits[r*2]<<8)|gl.bits[r*2+1]);
        if(!row) continue;
        int py=by+r; if(py<0||py>=g_ph) continue;
        uint32_t* tline=g_text.data()+(size_t)py*g_pw+bx;
        for(int k=0;k<maxc;k++){ int px=bx+k; if(px>=g_pw) break; if(row&(0x8000>>k)) tline[k]=fgc; }
      }
    }
  }
}
static void renderText(){ renderTextRows(0,g_H-1); }
static void compose(){
  uint32_t bg=g_bg, *fb=g_fb.data(); const uint32_t* ink=g_ink.data(); const uint32_t* tx=g_text.data();
  size_t n=(size_t)g_pw*g_ph;
  for(size_t i=0;i<n;i++){ uint32_t b=bg; uint32_t k=ink[i]; if(k>>24) b=over(b,k); uint32_t t=tx[i]; if(t>>24) b=over(b,t); fb[i]=b; }
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
  RECT c={r.left+g_ox, g_canvasY+r.top, r.right+1+g_ox, g_canvasY+r.bottom+1};
  InvalidateRect(g_hwnd,&c,FALSE);
}
static void recomposePx(const RECT& r0){
  RECT r=r0; clampPx(r); if(rectEmpty(r)) return;
  for(int y=r.top;y<=r.bottom;y++){
    size_t base=(size_t)y*g_pw;
    uint32_t* fb=g_fb.data()+base; const uint32_t* tx=g_text.data()+base; const uint32_t* ink=g_ink.data()+base;
    for(int x=r.left;x<=r.right;x++){ uint32_t b=g_bg; uint32_t k=ink[x]; if(k>>24) b=over(b,k); uint32_t t=tx[x]; if(t>>24) b=over(b,t); fb[x]=b; }
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
  if(!selAny()) return r;
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  if(c1<c0) return r;
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
static void invalidateStatus();                      // 前置声明（定义在下方）
static void overlayChanged(){
  invalidatePx(g_prevOverlay);
  g_prevOverlay=overlayRectPx();
  invalidatePx(g_prevOverlay);
  invalidateStatus();                                 // 光标动了 -> 底栏坐标同步
}
static void invalidateStatus(){ RECT c={0,g_ch-g_SB,g_cw,g_ch}; InvalidateRect(g_hwnd,&c,FALSE); }
static void invalidateTop(){ RECT c={0,0,g_cw,g_TB}; InvalidateRect(g_hwnd,&c,FALSE); }

// ---------------- 格子 ----------------
static void clearGlyphAt(int row,int col){
  if(col<0||col>=g_cols) return;
  int start=(g_cells[(size_t)row*g_cols+col]==CONT)? col-1 : col;
  if(start<0) return;
  uint32_t cp=g_cells[(size_t)row*g_cols+start];
  if(!cp||cp==CONT) return;
  Glyph gl=getGlyph(cp); int w=(gl.w==2)?2:1;
  g_cells[(size_t)row*g_cols+start]=0; g_cellColor[(size_t)row*g_cols+start]=0;
  if(w==2 && start+1<g_cols){ g_cells[(size_t)row*g_cols+start+1]=0; g_cellColor[(size_t)row*g_cols+start+1]=0; }
}
static void truncateFuture(){ if((int)g_ops.size()>g_pos) g_ops.resize(g_pos); }
static int timelineCap(){ int c=g_visW/16; return c<1?1:c; }
static void baseReset(){
  g_baseCells.assign((size_t)g_H*g_cols,0u);
  g_baseCellColor.assign((size_t)g_H*g_cols,0u);
  g_baseInk.assign((size_t)g_pw*g_ph,0u);
}
static void dropOldest(){
  if(g_ops.empty()) return;
  Op op=g_ops.front();
  if(op.type==OP_TEXT){
    for(int r=0;r<op.rows;r++)
      for(int c=0;c<op.cols;c++){
        int rr=op.r0+r, cc=op.c0+c;
        if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols){ g_baseCells[(size_t)rr*g_cols+cc]=op.after[(size_t)r*op.cols+c]; g_baseCellColor[(size_t)rr*g_cols+cc]=op.afterC[(size_t)r*op.cols+c]; }
      }
  } else if(op.type==OP_REGION){
    for(int r=0;r<op.rows;r++)
      for(int c=0;c<op.cols;c++){
        int rr=op.r0+r, cc=op.c0+c;
        if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols){ g_baseCells[(size_t)rr*g_cols+cc]=op.after[(size_t)r*op.cols+c]; g_baseCellColor[(size_t)rr*g_cols+cc]=op.afterC[(size_t)r*op.cols+c]; }
      }
    writeInkTo(g_baseInk.data(),op);
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
  size_t n=(size_t)op.rows*op.cols;
  op.before.assign(n,0u); op.beforeC.assign(n,0u);
  for(int r=0;r<op.rows;r++)
    for(int c=0;c<op.cols;c++){
      int rr=op.r0+r, cc=op.c0+c;
      if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols){ op.before[(size_t)r*op.cols+c]=g_cells[(size_t)rr*g_cols+cc]; op.beforeC[(size_t)r*op.cols+c]=g_cellColor[(size_t)rr*g_cols+cc]; }
    }
}
static void snapAfter(Op& op){
  size_t n=(size_t)op.rows*op.cols;
  op.after.assign(n,0u); op.afterC.assign(n,0u);
  for(int r=0;r<op.rows;r++)
    for(int c=0;c<op.cols;c++){
      int rr=op.r0+r, cc=op.c0+c;
      if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols){ op.after[(size_t)r*op.cols+c]=g_cells[(size_t)rr*g_cols+cc]; op.afterC[(size_t)r*op.cols+c]=g_cellColor[(size_t)rr*g_cols+cc]; }
    }
}
static void snapInkBefore(Op& op){
  int W=op.iw, H=op.ih; op.inkB.assign((size_t)W*H,0u);
  for(int y=0;y<H;y++) for(int x=0;x<W;x++){
    int px=op.ix+x, py=op.iy+y;
    if(px>=0&&px<g_pw&&py>=0&&py<g_ph) op.inkB[(size_t)y*W+x]=g_ink[(size_t)py*g_pw+px];
  }
}
static void snapInkAfter(Op& op){
  int W=op.iw, H=op.ih; op.inkA.assign((size_t)W*H,0u);
  for(int y=0;y<H;y++) for(int x=0;x<W;x++){
    int px=op.ix+x, py=op.iy+y;
    if(px>=0&&px<g_pw&&py>=0&&py<g_ph) op.inkA[(size_t)y*W+x]=g_ink[(size_t)py*g_pw+px];
  }
}
static void writeInkTo(uint32_t* dst,const Op& op){   // 把 op.inkA 写进目标位图
  int W=op.iw, H=op.ih;
  if(op.inkA.size()!=(size_t)W*H) return;
  for(int y=0;y<H;y++) for(int x=0;x<W;x++){
    int px=op.ix+x, py=op.iy+y;
    if(px>=0&&px<g_pw&&py>=0&&py<g_ph) dst[(size_t)py*g_pw+px]=op.inkA[(size_t)y*W+x];
  }
}
static void putGlyph(int row,int col,uint32_t cp,uint32_t color){
  Glyph gl=getGlyph(cp); int w=(gl.bits&&gl.w==2)?2:1;
  if(col+w>g_cols) w=1;
  clearGlyphAt(row,col);
  if(w==2) clearGlyphAt(row,col+1);
  g_cells[(size_t)row*g_cols+col]=cp; g_cellColor[(size_t)row*g_cols+col]=color;
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
  putGlyph(row,g_cx,cp,textColor());
  g_cx+=w; if(g_cx>g_cols-1) g_cx=g_cols-1; selCaret();
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
  g_cx=c; selCaret();
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
// 统一选区文本：矩形去掉行尾空格；单字(含空格)保留空格
static std::wstring selText(){
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  if(c1<c0) return L"";
  if(r0==r1 && c1-c0<=1){
    int cnt=0; uint32_t only=0;
    for(int c=c0;c<=c1;c++){ uint32_t v=g_cells[(size_t)r0*g_cols+c]; if(v&&v!=CONT){ cnt++; only=v; } }
    if(cnt<=1){ std::wstring out; appendCP(out, only?only:L' '); return out; }
  }
  std::wstring out;
  for(int r=r0;r<=r1;r++){
    std::wstring line;
    for(int c=c0;c<=c1;c++){
      if(!selGet(c,r)) continue;
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
// 私有格式 v3：'TXCB' + ver + rows + cols + anchorC + anchorR + cp[] + color[] + (iw,ih) + ink[iw*ih]
// anchorC/anchorR = blob 左上角相对"复制时光标"的偏移；粘贴时 blob 左上角 = 粘贴光标 + anchor
static void buildBlob(int c0,int c1,int r0,int r1,int anchorC,int anchorR,std::vector<uint8_t>& out){
  int rows=r1-r0+1, cols=c1-c0+1; size_t n=(size_t)rows*cols;
  int iw=cols*COLW, ih=rows*ROWH; size_t ni=(size_t)iw*ih;
  out.assign(24+n*8+8+ni*4,0);
  uint32_t* h=(uint32_t*)out.data(); h[0]=0x42435854u; h[1]=3u; h[2]=(uint32_t)rows; h[3]=(uint32_t)cols; h[4]=(uint32_t)anchorC; h[5]=(uint32_t)anchorR;
  uint32_t* cp=(uint32_t*)(out.data()+24);
  uint32_t* co=(uint32_t*)(out.data()+24+n*4);
  uint32_t* ip=(uint32_t*)(out.data()+24+n*8); ip[0]=(uint32_t)iw; ip[1]=(uint32_t)ih;
  uint32_t* ink=(uint32_t*)(out.data()+24+n*8+8);
  for(int r=0;r<rows;r++) for(int c=0;c<cols;c++){
    int rr=r0+r, cc=c0+c, ok=(rr<g_H&&cc<g_cols) && selGet(cc,rr);
    cp[(size_t)r*cols+c]= ok? g_cells[(size_t)rr*g_cols+cc] : 0;
    co[(size_t)r*cols+c]= ok? g_cellColor[(size_t)rr*g_cols+cc] : 0;
  }
  for(int y=0;y<ih;y++) for(int x=0;x<iw;x++){
    int px=c0*COLW+x, py=r0*ROWH+y;
    bool ok=(px<g_pw&&py<g_ph) && selGet(px/COLW, py/ROWH);
    ink[(size_t)y*iw+x]= ok? g_ink[(size_t)py*g_pw+px] : 0;
  }
}
// 文字(CF_UNICODETEXT) + 私有块 + (选区有墨迹时)图像(CF_DIB/PNG)
static void setClipAll(const std::wstring& text,const std::vector<uint8_t>& blob,int c0,int c1,int r0,int r1){
  int rw=(c1-c0+1)*COLW, rh=(r1-r0+1)*ROWH, x0=c0*COLW, y0=r0*ROWH;
  bool hasInk=false;
  for(int y=0;y<rh&&!hasInk;y++){ int py=y0+y; if(py>=g_ph) break;
    for(int x=0;x<rw;x++){ int px=x0+x; if(px>=g_pw) break;
      if(selGet(px/COLW, py/ROWH) && (g_ink[(size_t)py*g_pw+px]>>24)!=0){ hasInk=true; break; } } }
  std::vector<uint32_t> region;
  if(hasInk&&rw>0&&rh>0){ region.assign((size_t)rw*rh,0);
    for(int y=0;y<rh;y++){ int py=y0+y; for(int x=0;x<rw;x++){ int px=x0+x;
      region[(size_t)y*rw+x]=(px<g_pw&&py<g_ph&&selGet(px/COLW,py/ROWH))?g_fb[(size_t)py*g_pw+px]:0; } } }
  if(!OpenClipboard(g_hwnd)) return;
  EmptyClipboard();
  { size_t tb=(text.size()+1)*sizeof(wchar_t); HGLOBAL gt=GlobalAlloc(GMEM_MOVEABLE,tb);
    if(gt){ void* p=GlobalLock(gt); if(p){ memcpy(p,text.c_str(),tb); GlobalUnlock(gt); SetClipboardData(CF_UNICODETEXT,gt); } } }
  if(g_cfCells && !blob.empty()){ HGLOBAL gb=GlobalAlloc(GMEM_MOVEABLE,blob.size());
    if(gb){ void* p=GlobalLock(gb); if(p){ memcpy(p,blob.data(),blob.size()); GlobalUnlock(gb); SetClipboardData(g_cfCells,gb); } } }
  if(hasInk&&rw>0&&rh>0){
    size_t pix=(size_t)rw*rh*4, total=sizeof(BITMAPINFOHEADER)+pix;
    HGLOBAL gd=GlobalAlloc(GMEM_MOVEABLE,total);
    if(gd){ BITMAPINFOHEADER* bi=(BITMAPINFOHEADER*)GlobalLock(gd);
      memset(bi,0,sizeof(BITMAPINFOHEADER)); bi->biSize=sizeof(BITMAPINFOHEADER); bi->biWidth=rw; bi->biHeight=-rh;
      bi->biPlanes=1; bi->biBitCount=32; bi->biCompression=BI_RGB; bi->biSizeImage=(DWORD)pix;
      uint32_t* dst=(uint32_t*)((BYTE*)bi+sizeof(BITMAPINFOHEADER));
      for(int i=0;i<rw*rh;i++) dst[i]=over(0xFFFFFFFFu,region[(size_t)i]);   // DIB 拍平到白底(兼容老程序)
      GlobalUnlock(gd); SetClipboardData(CF_DIB,gd);
    }
    if(g_cfPNG){ Bitmap bmp(rw,rh,rw*4,PixelFormat32bppARGB,(BYTE*)region.data());
      IStream* st=nullptr; if(CreateStreamOnHGlobal(nullptr,TRUE,&st)==S_OK){
        CLSID cl; if(GetEncoderClsid(L"image/png",&cl)>=0 && bmp.Save(st,&cl,nullptr)==Gdiplus::Ok){
          STATSTG ss; memset(&ss,0,sizeof(ss)); st->Stat(&ss,STATFLAG_NONAME); size_t n=(size_t)ss.cbSize.QuadPart;
          HGLOBAL gp=GlobalAlloc(GMEM_MOVEABLE,n);
          if(gp){ void* p=GlobalLock(gp); LARGE_INTEGER z; z.QuadPart=0; st->Seek(z,STREAM_SEEK_SET,nullptr); ULONG rd=0; st->Read(p,(ULONG)n,&rd); GlobalUnlock(gp); SetClipboardData(g_cfPNG,gp); }
        }
        st->Release();
      }
    }
  }
  CloseClipboard();
}
static void doCopy(){
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  if(c1<c0) return;
  std::vector<uint8_t> blob; buildBlob(c0,c1,r0,r1,c0-g_cx,r0-g_cy,blob);
  setClipAll(selText(),blob,c0,c1,r0,r1);
}
static RECT doCut(){
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  if(c1<c0) return emptyRectPx();
  std::wstring txt=selText();
  { std::vector<uint8_t> bl; buildBlob(c0,c1,r0,r1,c0-g_cx,r0-g_cy,bl); setClipAll(txt,bl,c0,c1,r0,r1); }
  bool has=false;                          // 无字且无墨：只复制，不删除
  for(int r=r0;r<=r1&&!has;r++) for(int c=c0;c<=c1;c++){ if(!selGet(c,r)) continue; uint32_t v=g_cells[(size_t)r*g_cols+c]; if(v&&v!=CONT){ has=true; break; } }
  if(!has) for(int y=r0*ROWH;y<(r1+1)*ROWH&&y<g_ph&&!has;y++) for(int x=c0*COLW;x<(c1+1)*COLW&&x<g_pw;x++){ if(selGet(x/COLW,y/ROWH) && (g_ink[(size_t)y*g_pw+x]>>24)!=0){ has=true; break; } }
  if(!has) return emptyRectPx();
  truncateFuture();
  Op op; op.type=OP_REGION; op.r0=r0; op.c0=c0; op.rows=r1-r0+1; op.cols=c1-c0+1;
  op.ix=c0*COLW; op.iy=r0*ROWH; op.iw=op.cols*COLW; op.ih=op.rows*ROWH;
  snapBefore(op); snapInkBefore(op);
  for(int r=r0;r<=r1;r++) for(int c=c0;c<=c1;c++){ if(!selGet(c,r)) continue; g_cells[(size_t)r*g_cols+c]=0; g_cellColor[(size_t)r*g_cols+c]=0; }
  for(int y=r0*ROWH;y<(r1+1)*ROWH&&y<g_ph;y++) for(int x=c0*COLW;x<(c1+1)*COLW&&x<g_pw;x++){ if(selGet(x/COLW,y/ROWH)) g_ink[(size_t)y*g_pw+x]=0; }
  snapAfter(op); snapInkAfter(op);
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=c0; op.cby1=r0;
  pushOp(op);
  g_cx=c0; g_cy=r0; selCaret();
  renderTextRows(r0,r1);
  g_dirty=true;
  return rowsRectPx(r0,r1);
}
static RECT pasteBlob(int rows,int cols,const uint32_t* cp,const uint32_t* co,int iw,int ih,const uint32_t* ink,int anchorC=0,int anchorR=0){
  (void)ih;                                        // 高度由 rows 保证，不再直接使用
  int c0=g_cx+anchorC, r0=g_cy+anchorR;
  int csrc=0, rsrc=0;                              // 越界裁剪：负偏移部分跳过
  if(c0<0){ csrc=-c0; c0=0; }
  if(r0<0){ rsrc=-r0; r0=0; }
  int pr=imin(rows-rsrc,g_H-r0); if(pr<=0) return emptyRectPx();
  int pc=imin(cols-csrc,g_cols-c0); if(pc<=0) return emptyRectPx();
  truncateFuture();
  Op op; op.type=OP_REGION; op.r0=r0; op.c0=c0; op.rows=pr; op.cols=pc;
  op.ix=c0*COLW; op.iy=r0*ROWH; op.iw=pc*COLW; op.ih=pr*ROWH;
  snapBefore(op); snapInkBefore(op);
  // 文字：图章——只贴“有字”的格(空/续格不动目标 = 语义 b)
  for(int r=0;r<pr;r++) for(int c=0;c<pc;c++){
    uint32_t v=cp[(size_t)(rsrc+r)*cols+(csrc+c)];
    if(v==0||v==CONT) continue;
    putGlyph(r0+r,c0+c,v,co[(size_t)(rsrc+r)*cols+(csrc+c)]);
  }
  // 墨迹：图章——只贴不透明像素(b)
  for(int y=0;y<pr*ROWH;y++) for(int x=0;x<pc*COLW;x++){
    uint32_t a=ink[(size_t)(rsrc*ROWH+y)*iw+(csrc*COLW+x)];
    if((a>>24)==0) continue;
    int px=c0*COLW+x, py=r0*ROWH+y;
    if(px<g_pw&&py<g_ph) g_ink[(size_t)py*g_pw+px]=a;
  }
  snapAfter(op); snapInkAfter(op);
  int caretC=imin(g_cx+cols, g_cols-1);                       // 向右水平平移选区列跨度，行不动
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=caretC; op.cby1=g_cy;
  pushOp(op);
  g_cx=caretC; selCaret();
  renderTextRows(r0,r0+pr-1);
  g_dirty=true;
  return rowsRectPx(r0,r0+pr-1);
}
static bool argbFromBitmap(Bitmap& bmp,std::vector<uint32_t>& out,int& W,int& H){
  W=(int)bmp.GetWidth(); H=(int)bmp.GetHeight(); if(W<=0||H<=0) return false;
  Rect r(0,0,W,H); BitmapData bd;
  if(bmp.LockBits(&r,ImageLockModeRead,PixelFormat32bppARGB,&bd)!=Gdiplus::Ok) return false;
  out.assign((size_t)W*H,0);
  for(int y=0;y<H;y++){ const uint32_t* src=(const uint32_t*)((const uint8_t*)bd.Scan0+(size_t)y*bd.Stride);
    for(int x=0;x<W;x++) out[(size_t)y*W+x]=src[x]; }
  bmp.UnlockBits(&bd); return true;
}
static bool dibToARGB(const std::vector<BYTE>& dib,std::vector<uint32_t>& out,int& W,int& H){
  if(dib.size()<sizeof(BITMAPINFOHEADER)) return false;
  BITMAPINFO* bi=(BITMAPINFO*)dib.data();
  DWORD hsz=bi->bmiHeader.biSize;                       // 头部大小(40=DIB,124=DIBV5)
  if(hsz<sizeof(BITMAPINFOHEADER)||hsz>dib.size()) return false;
  int bc=bi->bmiHeader.biBitCount; size_t hdr=hsz; if(bc<=8) hdr+=((size_t)1<<bc)*sizeof(RGBQUAD);
  HDC dc=GetDC(g_hwnd); void* bits=nullptr; HBITMAP hb=CreateDIBSection(dc,bi,DIB_RGB_COLORS,&bits,nullptr,0); ReleaseDC(g_hwnd,dc);
  if(hb&&bits&&dib.size()>hdr){ memcpy(bits,dib.data()+hdr,dib.size()-hdr); Bitmap bmp(hb,nullptr);
    bool ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); DeleteObject(hb); return ok; }
  if(hb) DeleteObject(hb);
  return false;
}
static bool clipImageARGB(std::vector<uint32_t>& out,int& W,int& H){
  if(IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(g_hwnd)){       // 1) 文件
    std::wstring path;
    HANDLE h=GetClipboardData(CF_HDROP);
    if(h){ const BYTE* base=(const BYTE*)GlobalLock(h);
      if(base){ struct DF{ DWORD pFiles; POINT pt; BOOL fNC; BOOL fWide; };
        const DF* df=(const DF*)base; const wchar_t* s=(const wchar_t*)(base+df->pFiles);
        while(*s){ std::wstring p=s; s+=p.size()+1;
          std::wstring e; size_t d=p.find_last_of(L'.'); if(d!=std::wstring::npos) e=p.substr(d);
          for(size_t i=0;i<e.size();i++) e[i]=(wchar_t)towlower(e[i]);
          if(e==L".png"||e==L".bmp"||e==L".jpg"||e==L".jpeg"||e==L".gif"||e==L".tif"||e==L".tiff"){ path=p; break; } }
        GlobalUnlock(h); } }
    CloseClipboard();
    if(!path.empty()){ Bitmap bmp(path.c_str()); if(bmp.GetLastStatus()==Gdiplus::Ok) return argbFromBitmap(bmp,out,W,H); }
  }
  if(g_cfPNG && IsClipboardFormatAvailable(g_cfPNG) && OpenClipboard(g_hwnd)){   // 2) PNG 数据
    std::vector<BYTE> data; HANDLE h=GetClipboardData(g_cfPNG);
    if(h){ SIZE_T sz=GlobalSize(h); const void* p=GlobalLock(h); if(p){ data.assign((const BYTE*)p,(const BYTE*)p+sz); GlobalUnlock(h);} }
    CloseClipboard();
    if(!data.empty()){ HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,data.size());
      if(g){ void* p=GlobalLock(g); memcpy(p,data.data(),data.size()); GlobalUnlock(g); IStream* st=nullptr;
        if(CreateStreamOnHGlobal(g,TRUE,&st)==S_OK){ Bitmap bmp(st); bool ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); st->Release(); if(ok) return true; } else GlobalFree(g); } }
  }
  if(IsClipboardFormatAvailable(CF_DIB) && OpenClipboard(g_hwnd)){          // 3) DIB
    std::vector<BYTE> dib; HANDLE h=GetClipboardData(CF_DIB);
    if(h){ SIZE_T sz=GlobalSize(h); const void* p=GlobalLock(h); if(p){ dib.assign((const BYTE*)p,(const BYTE*)p+sz); GlobalUnlock(h);} }
    CloseClipboard();
    if(dibToARGB(dib,out,W,H)) return true;
  }
  return false;
}
// 图片粘贴：光标像素为左上角，1:1 不缩放，超出的忽略
static RECT pasteImageARGB(const std::vector<uint32_t>& img,int W,int H){
  int x0=g_cx*COLW, y0=g_cy*ROWH;
  int cw=(x0+W<=g_pw)? W : (g_pw-x0);
  int ch=(y0+H<=g_ph)? H : (g_ph-y0);
  if(cw<=0||ch<=0) return emptyRectPx();
  truncateFuture();
  Op op; op.type=OP_REGION; op.r0=0; op.c0=0; op.rows=0; op.cols=0;
  op.ix=x0; op.iy=y0; op.iw=cw; op.ih=ch;
  snapBefore(op); snapInkBefore(op);
  for(int y=0;y<ch;y++) for(int x=0;x<cw;x++){
    uint32_t v=img[(size_t)y*W+x];
    if((v>>24)==0) continue;                 // 透明不贴(图章)
    g_ink[(size_t)(y0+y)*g_pw+(x0+x)]=v;
  }
  snapAfter(op); snapInkAfter(op);
  op.cbx0=g_cx; op.cby0=g_cy;                               // 粘贴前光标
  g_cx=imin(g_cx+(cw+COLW-1)/COLW, g_cols-1);               // 向右平移图像覆盖列数，行不动
  selCaret();
  op.cbx1=g_cx; op.cby1=g_cy;
  pushOp(op);
  g_dirty=true;
  RECT r={x0,y0,x0+cw-1,y0+ch-1}; return r;
}
static RECT pasteImage(){
  std::vector<uint32_t> img; int W=0,H=0;
  if(!clipImageARGB(img,W,H)) return emptyRectPx();
  return pasteImageARGB(img,W,H);
}
static bool loadImageFileARGB(const wchar_t* path,std::vector<uint32_t>& out,int& W,int& H){
  Bitmap bmp(path); if(bmp.GetLastStatus()!=Gdiplus::Ok) return false;
  return argbFromBitmap(bmp,out,W,H);
}
static RECT pasteImageFile(const wchar_t* path){
  std::vector<uint32_t> img; int W=0,H=0;
  if(!loadImageFileARGB(path,img,W,H)) return emptyRectPx();
  return pasteImageARGB(img,W,H);
}
// 文本粘贴：从若干行文本(已解码 UTF-16)按当前色写入
static RECT pasteText(const std::wstring& t){
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
      if(cp<32) continue;
      Glyph gl=getGlyph(cp); int w=(gl.bits&&gl.w==2)?2:1;
      if(c+w>g_cols) break;
      putGlyph(r0+r,c,cp,textColor());
      c+=w;
    }
  }
  int caretC=imin(g_cx+cols, g_cols-1);   // 右移文本列宽，行不动
  snapAfter(op);
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=caretC; op.cby1=g_cy;
  pushOp(op);
  g_cx=caretC; selCaret();
  renderTextRows(r0,r0+rows-1);
  g_dirty=true;
  return rowsRectPx(r0,r0+rows-1);
}
static bool readTextFile(const wchar_t* path,std::wstring& out){
  FILE* f=_wfopen(path,L"rb"); if(!f) return false;
  std::vector<uint8_t> b; uint8_t buf[4096]; size_t n;
  while((n=fread(buf,1,sizeof(buf),f))>0) b.insert(b.end(),buf,buf+n);
  fclose(f);
  if(b.empty()){ out.clear(); return true; }
  if(b.size()>=3 && b[0]==0xEF && b[1]==0xBB && b[2]==0xBF){                 // UTF-8 BOM
    std::string s((const char*)b.data()+3, b.size()-3);
    int need=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
    std::wstring w(need,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],need);
    out.swap(w); return true;
  }
  if(b.size()>=2 && b[0]==0xFF && b[1]==0xFE){                                // UTF-16LE BOM
    size_t m=(b.size()-2)/2; std::wstring w(m,0);
    memcpy(&w[0],b.data()+2,m*2); out.swap(w); return true;
  }
  if(b.size()>=2 && b[0]==0xFE && b[1]==0xFF){                                // UTF-16BE BOM
    size_t m=(b.size()-2)/2; std::wstring w(m,0);
    for(size_t i=0;i<m;i++){ w[i]=(wchar_t)(((uint16_t)b[2+i*2]<<8)|b[2+i*2+1]); }
    out.swap(w); return true;
  }
  // 无 BOM：先试 UTF-8，失败退回 ANSI
  std::string s((const char*)b.data(), b.size());
  int need=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.c_str(),(int)s.size(),nullptr,0);
  if(need>0){ std::wstring w(need,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],need); out.swap(w); return true; }
  int na=MultiByteToWideChar(CP_ACP,0,s.c_str(),(int)s.size(),nullptr,0);
  std::wstring w(na>0?na:0,0); if(na>0) MultiByteToWideChar(CP_ACP,0,s.c_str(),(int)s.size(),&w[0],na);
  out.swap(w); return true;
}
static RECT pasteTextFile(const wchar_t* path){
  std::wstring t; if(!readTextFile(path,t)) return emptyRectPx();
  return pasteText(t);
}
static RECT doPaste(){
  if(g_cfCells && IsClipboardFormatAvailable(g_cfCells) && OpenClipboard(g_hwnd)){
    std::vector<uint8_t> blob;
    HANDLE h=GetClipboardData(g_cfCells);
    if(h){ SIZE_T sz=GlobalSize(h); const void* p=GlobalLock(h); if(p){ blob.assign((const uint8_t*)p,(const uint8_t*)p+sz); GlobalUnlock(h);} }
    CloseClipboard();
    if(blob.size()>=24){
      const uint32_t* hh=(const uint32_t*)blob.data();
      int rows=(int)hh[2], cols=(int)hh[3];
      if(hh[0]==0x42435854u && hh[1]==3u && rows>0 && cols>0){
        size_t n=(size_t)rows*cols;
        const uint32_t* cp=(const uint32_t*)(blob.data()+24);
        const uint32_t* co=(const uint32_t*)(blob.data()+24+n*4);
        const uint32_t* ihp=(const uint32_t*)(blob.data()+24+n*8);
        int iw=(int)ihp[0], ihh=(int)ihp[1];
        const uint32_t* ink=(const uint32_t*)(blob.data()+24+n*8+8);
        int ac=(int)hh[4], ar=(int)hh[5];
        if(iw>0&&ihh>0 && blob.size()>=24+n*8+8+(size_t)iw*ihh*4) return pasteBlob(rows,cols,cp,co,iw,ihh,ink,ac,ar);
      }
    }
  }
  if(IsClipboardFormatAvailable(CF_HDROP) && OpenClipboard(g_hwnd)){          // 剪贴板里的文件
    std::vector<std::wstring> fs; HANDLE hd=GetClipboardData(CF_HDROP);
    if(hd) fs=hdropFiles(hd);
    CloseClipboard();
    for(auto& p:fs){ RECT r=pasteDroppedFile(p.c_str()); if(!rectEmpty(r)) return r; }
  }
  { RECT r=pasteImage(); if(!rectEmpty(r)) return r; }   // 图片(HDROP/PNG/DIB)
  if(!IsClipboardFormatAvailable(CF_UNICODETEXT)) return emptyRectPx();
  if(!OpenClipboard(g_hwnd)) return emptyRectPx();
  std::wstring t;
  HANDLE h=GetClipboardData(CF_UNICODETEXT);
  if(h){ const wchar_t* p=(const wchar_t*)GlobalLock(h); if(p){ t=p; GlobalUnlock(h);} }
  CloseClipboard();
  return pasteText(t);
}

// ---------------- 画笔 ----------------
static void stampBuf(uint32_t* buf,int cx,int cy,int r,uint32_t col){
  int y0=cy-r; if(y0<0)y0=0; int y1=cy+r; if(y1>g_ph-1)y1=g_ph-1;
  int x0=cx-r; if(x0<0)x0=0; int x1=cx+r; if(x1>g_pw-1)x1=g_pw-1;
  for(int y=y0;y<=y1;y++){
    int dy=y-cy, dy2=dy*dy;
    for(int x=x0;x<=x1;x++){
      int dx=x-cx;
      if(dx*dx+dy2>r*r) continue;
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
  size_t si=0;
  for(int k=0;k<g_pos;k++){
    Op& op=g_ops[k];
    if(op.type==OP_STROKE){
      if(si<g_strokes.size()){
        Stroke& st=g_strokes[si++]; int r=st.size/2;
        if(st.pts.size()==1) stampBuf(g_ink.data(),st.pts[0].x,st.pts[0].y,r,st.color);
        for(size_t i=1;i<st.pts.size();i++)
          lineBuf(g_ink.data(),st.pts[i-1].x,st.pts[i-1].y,st.pts[i].x,st.pts[i].y,r,st.color);
      }
    } else if(op.type==OP_REGION){
      writeInkTo(g_ink.data(),op);
    }
  }
}

// ---------------- 历史时间轴 ----------------
static void rebuildState(int p){
  for(size_t i=0;i<g_cells.size();i++){ g_cells[i]=g_baseCells[i]; g_cellColor[i]=g_baseCellColor[i]; }
  g_strokes.clear();
  for(int k=0;k<p;k++){
    Op& op=g_ops[k];
    if(op.type==OP_TEXT || op.type==OP_REGION){
      for(int r=0;r<op.rows;r++)
        for(int c=0;c<op.cols;c++){
          int rr=op.r0+r, cc=op.c0+c;
          if(rr>=0&&rr<g_H&&cc>=0&&cc<g_cols){ g_cells[(size_t)rr*g_cols+cc]=op.after[(size_t)r*op.cols+c]; g_cellColor[(size_t)rr*g_cols+cc]=op.afterC[(size_t)r*op.cols+c]; }
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
  g_pos=p; g_selecting=false; rebuildState(g_pos); g_dirty=true; selCaret();
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
  int y0=g_ch-g_SB;
  for(int y=y0;y<g_ch;y++)
    for(int x=0;x<g_cw;x++) g_memBits[(size_t)y*g_cw+x]=0xFFF0F0F0u;
  for(int x=0;x<g_cw;x++) putPx(x,y0,0xFF808080u);
  // 光标坐标 y,x（各固定 3 位，超出显示 ***）
  wchar_t sy[4]={L'0',L'0',L'0',0}, sx[4]={L'0',L'0',L'0',0};
  if(g_cy<1000) swprintf(sy,4,L"%03d",g_cy); else wcscpy(sy,L"***");
  if(g_cx<1000) swprintf(sx,4,L"%03d",g_cx); else wcscpy(sx,L"***");
  uint32_t cc=0xFF202020u;
  int px=0;
  for(int i=0;i<3;i++){ blitGlyph(sy[i],px,y0+1,cc); px+=8; }
  blitGlyph(L',',px,y0+1,cc); px+=8;
  for(int i=0;i<3;i++){ blitGlyph(sx[i],px,y0+1,cc); px+=8; }
  blitGlyph(L'|',px,y0+1,0xFF808080u); px+=8;
  // 时间轴（从坐标区右侧开始）
  uint32_t act=0xFF202020u, gray=0xFFB4B4B4u;
  int n=(int)g_ops.size(); int cap=(g_cw-px)/16; if(n>cap) n=cap;
  for(int i=0;i<n;i++){
    uint32_t col=(i<g_pos)?act:gray;
    uint32_t cp=(g_ops[i].type==OP_TEXT)?CP_WRITE:CP_DRAW;
    blitGlyph(cp, px+i*16, y0+1, col);
  }
}
static void drawSelection(){
  for(int r=0;r<g_H;r++) for(int c=0;c<g_cols;c++){
    if(!g_selMask[(size_t)r*g_cols+c]) continue;
    int bx=g_ox+c*COLW, by=g_canvasY+r*ROWH;
    for(int y=0;y<ROWH;y++) for(int x=0;x<COLW;x++) invertPx(bx+x,by+y);
  }
}
static void drawCaret(){
  int row=g_cy, col=g_cx, w=1;
  uint32_t cp=g_cells[(size_t)row*g_cols+g_cx];
  if(cp==CONT){ col=g_cx-1; w=2; }
  else if(cp){ Glyph gl=getGlyph(cp); w=(gl.w==2)?2:1; }
  int bx=g_ox+col*COLW, by=g_canvasY+row*ROWH;
  for(int y=0;y<ROWH;y++) for(int x=0;x<w*COLW;x++) invertPx(bx+x,by+y);
}
// 光标是否落在选区内：是则不再单独画（选区即"放大的光标"）
static bool caretInSel(){
  int c=g_cx; uint32_t cp=g_cells[(size_t)g_cy*g_cols+c];
  if(cp==CONT) c--;
  if(c<0) return false;
  if(!selGet(c,g_cy)) return false;
  cp=g_cells[(size_t)g_cy*g_cols+c];
  if(cp&&cp!=CONT){ Glyph gl=getGlyph(cp); if(gl.w==2) return c+1<g_cols && selGet(c+1,g_cy); }
  return true;
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
// 改色时把"当前选区"里的字一并改成该色（一次“写”）
static void recolorSelection(uint32_t color){
  int c0,c1,r0,r1; selBounds(c0,c1,r0,r1);
  if(c1<c0) return;
  bool any=false;
  for(int r=r0;r<=r1&&!any;r++) for(int c=c0;c<=c1;c++){ if(!selGet(c,r)) continue; uint32_t v=g_cells[(size_t)r*g_cols+c]; if(v&&v!=CONT){ any=true; break; } }
  if(!any) return;
  truncateFuture();
  Op op; op.type=OP_TEXT; op.r0=r0; op.c0=c0; op.rows=r1-r0+1; op.cols=c1-c0+1;
  snapBefore(op);
  for(int r=r0;r<=r1;r++) for(int c=c0;c<=c1;c++){ if(!selGet(c,r)) continue; uint32_t v=g_cells[(size_t)r*g_cols+c]; if(v&&v!=CONT) g_cellColor[(size_t)r*g_cols+c]=color; }
  snapAfter(op);
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=g_cx; op.cby1=g_cy;
  pushOp(op);
  renderTextRows(r0,r1);
  g_dirty=true;
  commitRect(rowsRectPx(r0,r1));
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
  recolorSelection(g_brush); invalidateTop();
}
static void commitSize(){
  wchar_t b[32]; GetWindowTextW(g_hEditSize,b,32);
  int v=_wtoi(b); if(v<1) v=1; if(v>g_maxSize) v=g_maxSize;
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
  recolorSelection(g_brush); invalidateTop();
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
  int bx=g_ox+g_cx*COLW, by=g_canvasY+g_cy*ROWH;
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
static bool writePNGFile(const wchar_t* path){
  Bitmap bmp(g_pw,g_ph,g_pw*4,PixelFormat32bppARGB,(BYTE*)g_fb.data());
  CLSID clsid;
  if(GetEncoderClsid(L"image/png",&clsid)<0) return false;
  if(bmp.Save(path,&clsid,nullptr)!=Gdiplus::Ok) return false;
  g_dirty=false;
  return true;
}

// ---------------- .texel.zip 格式（自写最小 ZIP: 仅 STORED） ----------------
static uint32_t crc32buf(const uint8_t* p,size_t n){
  static uint32_t t[256]; static bool in=false;
  if(!in){ for(uint32_t i=0;i<256;i++){ uint32_t c=i; for(int k=0;k<8;k++) c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1); t[i]=c; } in=true; }
  uint32_t c=0xFFFFFFFFu; for(size_t i=0;i<n;i++) c=t[(c^p[i])&0xFF]^(c>>8); return c^0xFFFFFFFFu;
}
static void put32(std::vector<uint8_t>& v,uint32_t x){ v.push_back(x&0xFF); v.push_back((x>>8)&0xFF); v.push_back((x>>16)&0xFF); v.push_back((x>>24)&0xFF); }
static void put16(std::vector<uint8_t>& v,uint16_t x){ v.push_back(x&0xFF); v.push_back((x>>8)&0xFF); }
static uint32_t get32(const uint8_t* p){ return (uint32_t)(p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24)); }
static uint16_t get16(const uint8_t* p){ return (uint16_t)(p[0]|(p[1]<<8)); }
struct ZipEntry { std::string name; std::vector<uint8_t> data; };
static std::vector<uint8_t> zipBuild(const std::vector<ZipEntry>& es){
  std::vector<uint8_t> out; std::vector<uint32_t> off,crc,sz;
  for(auto& e:es){
    off.push_back((uint32_t)out.size());
    uint32_t c=crc32buf(e.data.data(),e.data.size()); crc.push_back(c); sz.push_back((uint32_t)e.data.size());
    put32(out,0x04034b50); put16(out,20); put16(out,0); put16(out,0); put16(out,0); put16(out,0);
    put32(out,c); put32(out,(uint32_t)e.data.size()); put32(out,(uint32_t)e.data.size());
    put16(out,(uint16_t)e.name.size()); put16(out,0);
    out.insert(out.end(),e.name.begin(),e.name.end());
    out.insert(out.end(),e.data.begin(),e.data.end());
  }
  uint32_t cdStart=(uint32_t)out.size();
  for(size_t i=0;i<es.size();i++){
    put32(out,0x02014b50); put16(out,20); put16(out,20); put16(out,0); put16(out,0); put16(out,0); put16(out,0);
    put32(out,crc[i]); put32(out,sz[i]); put32(out,sz[i]);
    put16(out,(uint16_t)es[i].name.size()); put16(out,0); put16(out,0); put16(out,0); put16(out,0); put32(out,0); put32(out,off[i]);
    out.insert(out.end(),es[i].name.begin(),es[i].name.end());
  }
  uint32_t cdSize=(uint32_t)out.size()-cdStart;
  put32(out,0x06054b50); put16(out,0); put16(out,0); put16(out,(uint16_t)es.size()); put16(out,(uint16_t)es.size());
  put32(out,cdSize); put32(out,cdStart); put16(out,0);
  return out;
}
static bool zipRead(const std::vector<uint8_t>& z,std::vector<ZipEntry>& es){
  if(z.size()<22) return false;
  size_t eocd=(size_t)-1;
  for(size_t i=z.size()-22;;i--){ if(get32(&z[i])==0x06054b50){ eocd=i; break; } if(i==0) break; }
  if(eocd==(size_t)-1) return false;
  uint16_t n=get16(&z[eocd+10]); uint32_t cd=get32(&z[eocd+16]); size_t p=cd;
  for(uint16_t i=0;i<n;i++){
    if(p+46>z.size()||get32(&z[p])!=0x02014b50) return false;
    uint32_t compSize=get32(&z[p+20]);
    uint16_t nlen=get16(&z[p+28]), elen=get16(&z[p+30]), clen=get16(&z[p+32]);
    uint32_t lho=get32(&z[p+42]);
    std::string name((const char*)&z[p+46],nlen);
    if((size_t)lho+30>z.size()||get32(&z[lho])!=0x04034b50) return false;
    uint16_t lnlen=get16(&z[lho+26]), lelen=get16(&z[lho+28]);
    size_t dstart=(size_t)lho+30+lnlen+lelen;
    if(dstart+compSize>z.size()) return false;
    ZipEntry e; e.name=name; e.data.assign(z.begin()+dstart,z.begin()+dstart+compSize);
    es.push_back(e);
    p+=(size_t)46+nlen+elen+clen;
  }
  return true;
}
static std::string utf8enc(const std::wstring& w){
  if(w.empty()) return std::string();
  int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
  std::string s(n,0); WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,nullptr,nullptr); return s;
}
static std::wstring utf8dec(const std::string& s){
  if(s.empty()) return std::wstring();
  int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
  std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n); return w;
}
static bool encodePNG(const uint32_t* argb,int w,int h,std::vector<uint8_t>& out){
  if(w<=0||h<=0) return false;
  Bitmap bmp(w,h,w*4,PixelFormat32bppARGB,(BYTE*)argb);
  IStream* st=nullptr; if(CreateStreamOnHGlobal(nullptr,TRUE,&st)!=S_OK) return false;
  CLSID cl; bool ok=false;
  if(GetEncoderClsid(L"image/png",&cl)>=0 && bmp.Save(st,&cl,nullptr)==Gdiplus::Ok){
    STATSTG ss; memset(&ss,0,sizeof(ss)); st->Stat(&ss,STATFLAG_NONAME); size_t n=(size_t)ss.cbSize.QuadPart;
    out.resize(n); LARGE_INTEGER z; z.QuadPart=0; st->Seek(z,STREAM_SEEK_SET,nullptr); ULONG rd=0; st->Read(out.data(),(ULONG)n,&rd); ok=true;
  }
  st->Release(); return ok;
}
static bool decodePNG(const std::vector<uint8_t>& png,std::vector<uint32_t>& out,int& W,int& H){
  HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,png.size()); if(!g) return false;
  void* p=GlobalLock(g); memcpy(p,png.data(),png.size()); GlobalUnlock(g);
  IStream* st=nullptr; if(CreateStreamOnHGlobal(g,TRUE,&st)!=S_OK){ GlobalFree(g); return false; }
  Bitmap bmp(st); bool ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); st->Release(); return ok;
}
static bool writeTexelZipFile(const wchar_t* path){
  std::string txt,col;
  for(int y=0;y<g_H;y++){
    std::wstring line,tok;
    for(int c=0;c<g_cols;c++){
      uint32_t cp=g_cells[(size_t)y*g_cols+c];
      if(cp==0||cp==CONT) continue;
      line.push_back((wchar_t)cp);
      uint32_t cc=g_cellColor[(size_t)y*g_cols+c];
      wchar_t tb[32];
      if((cc>>24)==0) swprintf(tb,32,L"%d:t",c); else swprintf(tb,32,L"%d:%06X",c,(unsigned)(cc&0xFFFFFF));
      if(!tok.empty()) tok.push_back(L' ');
      tok+=tb;
    }
    line.push_back(L'\n'); tok.push_back(L'\n');
    txt+=utf8enc(line); col+=utf8enc(tok);
  }
  std::vector<uint8_t> png; if(!encodePNG(g_ink.data(),g_pw,g_ph,png)) return false;
  wchar_t bb[16]; if((g_bg>>24)==0) wcscpy(bb,L"trans"); else swprintf(bb,16,L"%06X",(unsigned)(g_bg&0xFFFFFF));
  std::string bgs=utf8enc(bb);
  std::vector<ZipEntry> es;
  es.push_back({"text.txt", std::vector<uint8_t>(txt.begin(),txt.end())});
  es.push_back({"txt.color",std::vector<uint8_t>(col.begin(),col.end())});
  es.push_back({"ink.png",  png});
  es.push_back({"bg.color", std::vector<uint8_t>(bgs.begin(),bgs.end())});
  std::vector<uint8_t> zip=zipBuild(es);
  FILE* f=_wfopen(path,L"wb"); if(!f) return false;
  fwrite(zip.data(),1,zip.size(),f); fclose(f);
  g_dirty=false; return true;
}
// 统一保存：一个对话框，两种格式，默认 .texel.zip
static bool saveFile(){
  wchar_t path[MAX_PATH]=L"untitled.texel.zip";
  OPENFILENAMEW ofn; memset(&ofn,0,sizeof(ofn)); ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=g_hwnd;
  ofn.lpstrFilter=L"texel (*.texel.zip)\0*.texel.zip\0PNG (*.png)\0*.png\0";
  ofn.nFilterIndex=1; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
  ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_EXPLORER;
  if(!GetSaveFileNameW(&ofn)) return false;
  std::wstring p=path, lo=p; for(auto&ch:lo) ch=(wchar_t)towlower(ch);
  auto ends=[&](const wchar_t* e){ std::wstring es=e; return lo.size()>=es.size()&&lo.compare(lo.size()-es.size(),es.size(),es)==0; };
  bool wantPng=(ofn.nFilterIndex==2);
  if(ends(L".png")) wantPng=true; else if(ends(L".zip")) wantPng=false;
  if(wantPng){ if(!ends(L".png")) p+=L".png"; return writePNGFile(p.c_str()); }
  if(!ends(L".zip")) p+=L".texel.zip";
  return writeTexelZipFile(p.c_str());
}
static bool readTexelZip(const wchar_t* path,std::vector<uint8_t>& tb,std::vector<uint8_t>& cb,std::vector<uint32_t>& ink,int& iw,int& ih,uint32_t* bg=nullptr){
  FILE* f=_wfopen(path,L"rb"); if(!f) return false;
  fseek(f,0,SEEK_END); long zs=ftell(f); fseek(f,0,SEEK_SET);
  std::vector<uint8_t> z(zs>0?zs:0); if(zs>0) fread(z.data(),1,zs,f); fclose(f);
  std::vector<ZipEntry> es; if(!zipRead(z,es)) return false;
  const std::vector<uint8_t>* png=nullptr; const std::vector<uint8_t>* tbp=nullptr; const std::vector<uint8_t>* cbp=nullptr; const std::vector<uint8_t>* bgp=nullptr;
  for(auto& e:es){ if(e.name=="ink.png") png=&e.data; else if(e.name=="text.txt") tbp=&e.data; else if(e.name=="txt.color") cbp=&e.data; else if(e.name=="bg.color") bgp=&e.data; }
  if(!png) return false;
  if(!decodePNG(*png,ink,iw,ih)) return false;
  if(tbp) tb=*tbp; if(cbp) cb=*cbp;
  if(bg){
    *bg=0x00FFFFFF;
    if(bgp && !bgp->empty()){
      std::wstring s=utf8dec(std::string(bgp->begin(),bgp->end()));
      std::wstring lo=s; for(auto&ch:lo) ch=(wchar_t)towlower(ch);
      if(!lo.empty() && lo!=L"trans"){ uint32_t c; if(parseColor(s.c_str(),&c)) *bg=0xFF000000u|c; }
    }
  }
  return true;
}
static void parseTextInto(std::vector<uint32_t>& cells,std::vector<uint32_t>& colors,int stride,int cols,int H,const std::vector<uint8_t>& tb,const std::vector<uint8_t>& cb){
  if(tb.empty()) return;
  std::vector<std::string> tl,cl;
  { std::string s((const char*)tb.data(),tb.size()); size_t i=0; while(i<=s.size()){ size_t j=s.find('\n',i); if(j==std::string::npos){ tl.push_back(s.substr(i)); break;} tl.push_back(s.substr(i,j-i)); i=j+1; } }
  if(!cb.empty()){ std::string s((const char*)cb.data(),cb.size()); size_t i=0; while(i<=s.size()){ size_t j=s.find('\n',i); if(j==std::string::npos){ cl.push_back(s.substr(i)); break;} cl.push_back(s.substr(i,j-i)); i=j+1; } }
  for(int y=0;y<H&&y<(int)tl.size();y++){
    std::wstring w=utf8dec(tl[y]); std::vector<int> tcol; std::vector<uint32_t> tcolor;
    if(y<(int)cl.size()){ const std::string& line=cl[y]; size_t i2=0;
      while(i2<line.size()){ while(i2<line.size()&&line[i2]==' ')i2++; if(i2>=line.size())break;
        size_t j2=line.find(' ',i2); std::string t=line.substr(i2,j2==std::string::npos?std::string::npos:j2-i2); i2=(j2==std::string::npos)?line.size():j2+1;
        size_t k=t.find(':'); if(k==std::string::npos)continue;
        tcol.push_back(atoi(t.substr(0,k).c_str()));
        std::string cs=t.substr(k+1); uint32_t ccc=0xFF000000u;
        if(cs!="t"){ unsigned v=0; sscanf(cs.c_str(),"%x",&v); ccc=0xFF000000u|(v&0xFFFFFFu); }
        tcolor.push_back(ccc); } }
    int cur=0;
    for(size_t ci=0;ci<w.size();ci++){
      uint32_t cp=w[ci]; Glyph gl=getGlyph(cp); int wd=(gl.bits&&gl.w==2)?2:1;
      int c=(ci<tcol.size())?tcol[ci]:cur;
      uint32_t ccc=(ci<tcolor.size())?tcolor[ci]:argb(g_fg);
      if(c>=0&&c<cols){ cells[(size_t)y*stride+c]=cp; if(wd==2&&c+1<cols) cells[(size_t)y*stride+c+1]=CONT; colors[(size_t)y*stride+c]=ccc; }
      cur=c+wd;
    }
  }
}
static void openTexelZipFile(const wchar_t* path){
  std::vector<uint8_t> tb,cb; std::vector<uint32_t> ink; int iw=0,ih=0;
  uint32_t bg=0x00FFFFFF;
  if(!readTexelZip(path,tb,cb,ink,iw,ih,&bg)) return;
  if(iw%COLW||ih%ROWH) return;
  int cols=iw/COLW, W=cols, H=ih/ROWH;
  if(W<2||H<1) return;
  g_bg=bg; g_fg=0x000000;
  setupCanvas(W,H,g_bg,g_fg);
  if((int)ink.size()==g_pw*g_ph) g_ink=ink;
  parseTextInto(g_cells,g_cellColor,g_cols,g_cols,H,tb,cb);
  g_baseCells=g_cells; g_baseCellColor=g_cellColor; g_baseInk=g_ink;
  g_ops.clear(); g_pos=0; g_strokes.clear();
  renderText(); compose(); g_dirty=false;
  InvalidateRect(g_hwnd,nullptr,FALSE); overlayChanged();
}
// 把 .texel.zip 内容“贴”进当前画布（拖动/粘贴用）
static RECT pasteTexelZipFile(const wchar_t* path){
  std::vector<uint8_t> tb,cb; std::vector<uint32_t> ink; int iw=0,ih=0;
  if(!readTexelZip(path,tb,cb,ink,iw,ih)) return emptyRectPx();
  if(iw%COLW||ih%ROWH) return emptyRectPx();
  int scols=iw/COLW, sh=ih/ROWH;
  std::vector<uint32_t> scells((size_t)sh*scols,0u),scolors((size_t)sh*scols,0u);
  parseTextInto(scells,scolors,scols,scols,sh,tb,cb);
  int c0=g_cx, r0=g_cy;
  int tcols=imin(scols,g_cols-c0); if(tcols<=0) return emptyRectPx();
  int th=imin(sh,g_H-r0);           if(th<=0) return emptyRectPx();
  int ix=c0*COLW, iy=r0*ROWH, tiw=imin(iw,g_pw-ix), tih=imin(ih,g_ph-iy);
  truncateFuture();
  Op op; op.type=OP_REGION; op.r0=r0; op.c0=c0; op.rows=th; op.cols=imin(tcols+1,g_cols-c0);
  op.ix=ix; op.iy=iy; op.iw=tiw; op.ih=tih;
  snapBefore(op); snapInkBefore(op);
  for(int r=0;r<th;r++) for(int c=0;c<tcols;c++){ uint32_t v=scells[(size_t)r*scols+c]; if(v==0||v==CONT) continue; putGlyph(r0+r,c0+c,v,scolors[(size_t)r*scols+c]); }
  for(int y=0;y<tih;y++) for(int x=0;x<tiw;x++){ uint32_t a=ink[(size_t)y*iw+x]; if((a>>24)==0) continue; g_ink[(size_t)(iy+y)*g_pw+(ix+x)]=a; }
  snapAfter(op); snapInkAfter(op);
  int caretC=imin(g_cx+tcols, g_cols-1);                      // 向右水平平移贴入列数，行不动
  op.cbx0=g_cx; op.cby0=g_cy; op.cbx1=caretC; op.cby1=g_cy;
  pushOp(op);
  g_cx=caretC; selCaret();
  renderTextRows(r0,r0+th-1); g_dirty=true;
  RECT rr={ix,iy,ix+tiw-1,iy+tih-1}; return rr;
}
static std::vector<std::wstring> hdropFiles(HANDLE h){
  std::vector<std::wstring> v; HDROP dr=(HDROP)h;
  UINT n=DragQueryFileW(dr,0xFFFFFFFF,nullptr,0);
  for(UINT i=0;i<n;i++){ wchar_t p[MAX_PATH]; if(DragQueryFileW(dr,i,p,MAX_PATH)) v.push_back(p); }
  return v;
}
static bool endsWithZW(const std::wstring& s,const wchar_t* suf){
  std::wstring lo=s; for(auto&ch:lo) ch=(wchar_t)towlower(ch);
  std::wstring e=suf; return lo.size()>=e.size() && lo.compare(lo.size()-e.size(),e.size(),e)==0;
}
static bool isImageExtZW(const std::wstring& s){
  return endsWithZW(s,L".png")||endsWithZW(s,L".bmp")||endsWithZW(s,L".jpg")||endsWithZW(s,L".jpeg")
      ||endsWithZW(s,L".gif")||endsWithZW(s,L".tif")||endsWithZW(s,L".tiff");
}
static bool isTextExtZW(const std::wstring& s){
  return endsWithZW(s,L".txt")||endsWithZW(s,L".md")||endsWithZW(s,L".markdown")
      ||endsWithZW(s,L".csv")||endsWithZW(s,L".json")||endsWithZW(s,L".log")||endsWithZW(s,L".xml");
}
// 按文件类型分发：.texel.zip / 图片 / 文本 -> 粘贴
static RECT pasteDroppedFile(const wchar_t* p){
  if(endsWithZW(p,L".texel.zip")) return pasteTexelZipFile(p);
  if(isImageExtZW(p)) return pasteImageFile(p);
  if(isTextExtZW(p)) return pasteTextFile(p);
  return emptyRectPx();
}

// ---------------- OLE 拖放(外部软件文本/图像选区拖动) ----------------
static bool idfHasSupported(IDataObject* pdo){
  FORMATETC fe; memset(&fe,0,sizeof(fe)); fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
  fe.cfFormat=CF_UNICODETEXT; if(pdo->QueryGetData(&fe)==S_OK) return true;
  fe.cfFormat=CF_TEXT; if(pdo->QueryGetData(&fe)==S_OK) return true;
  fe.cfFormat=CF_DIB; if(pdo->QueryGetData(&fe)==S_OK) return true;
  fe.cfFormat=CF_HDROP; if(pdo->QueryGetData(&fe)==S_OK) return true;
  if(g_cfPNG){ fe.cfFormat=g_cfPNG; if(pdo->QueryGetData(&fe)==S_OK) return true; }
  if(g_cfDragImageBits){ fe.cfFormat=g_cfDragImageBits; if(pdo->QueryGetData(&fe)==S_OK) return true; }
  return false;
}
static bool idfReadText(IDataObject* pdo,std::wstring& out){
  FORMATETC fe; STGMEDIUM med;
  memset(&fe,0,sizeof(fe)); fe.cfFormat=CF_UNICODETEXT; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
  memset(&med,0,sizeof(med));
  if(pdo->GetData(&fe,&med)==S_OK && med.hGlobal){
    SIZE_T sz=GlobalSize(med.hGlobal); const wchar_t* p=(const wchar_t*)GlobalLock(med.hGlobal);
    if(p){ size_t n=sz/2; while(n>0 && p[n-1]==0) n--; out.assign(p,p+n); GlobalUnlock(med.hGlobal); }
    ReleaseStgMedium(&med);
    if(!out.empty()) return true;
  }
  memset(&fe,0,sizeof(fe)); fe.cfFormat=CF_TEXT; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
  memset(&med,0,sizeof(med));
  if(pdo->GetData(&fe,&med)==S_OK && med.hGlobal){
    SIZE_T sz=GlobalSize(med.hGlobal); const char* p=(const char*)GlobalLock(med.hGlobal);
    if(p){ size_t len=sz; while(len>0 && p[len-1]==0) len--;
      int n=MultiByteToWideChar(CP_ACP,0,p,(int)len,nullptr,0);
      if(n>0){ out.assign((size_t)n,0); MultiByteToWideChar(CP_ACP,0,p,(int)len,&out[0],n); }
      GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return !out.empty();
  }
  return false;
}
struct SDragImage { SIZE sz; POINT pt; HBITMAP hbmp; COLORREF key; };
static bool idfReadFiles(IDataObject* pdo,std::vector<std::wstring>& fs){
  FORMATETC fe; STGMEDIUM med;
  memset(&fe,0,sizeof(fe)); fe.cfFormat=CF_HDROP; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
  memset(&med,0,sizeof(med));
  if(pdo->GetData(&fe,&med)!=S_OK || !med.hGlobal){ ReleaseStgMedium(&med); return false; }
  fs=hdropFiles(med.hGlobal);
  ReleaseStgMedium(&med);
  return !fs.empty();
}
static bool idfReadFileContentsImage(IDataObject* pdo,int lindex,std::vector<uint32_t>& out,int& W,int& H){
  for(int t=0;t<2;t++){
    FORMATETC fe; STGMEDIUM med;
    memset(&fe,0,sizeof(fe)); fe.cfFormat=g_cfFileContents; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=lindex;
    fe.tymed=(t==0)?TYMED_ISTREAM:TYMED_HGLOBAL;
    memset(&med,0,sizeof(med));
    if(pdo->GetData(&fe,&med)==S_OK){
      bool ok=false;
      if(med.tymed==TYMED_ISTREAM && med.pstm){ Bitmap bmp(med.pstm); ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); }
      else if(med.tymed==TYMED_HGLOBAL && med.hGlobal){
        SIZE_T sz=GlobalSize(med.hGlobal); const void* p=GlobalLock(med.hGlobal);
        if(p){ std::vector<BYTE> data; data.assign((const BYTE*)p,(const BYTE*)p+sz); GlobalUnlock(med.hGlobal);
          if(!data.empty()){ HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,data.size());
            if(g){ void* q=GlobalLock(g); memcpy(q,data.data(),data.size()); GlobalUnlock(g); IStream* st=nullptr;
              if(CreateStreamOnHGlobal(g,TRUE,&st)==S_OK){ Bitmap bmp(st); ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); st->Release(); } else GlobalFree(g); } } }
      }
      ReleaseStgMedium(&med);
      if(ok) return true;
    }
  }
  return false;
}
static bool idfReadImageARGB(IDataObject* pdo,std::vector<uint32_t>& out,int& W,int& H){
  FORMATETC fe; STGMEDIUM med;
  // 1) CF_DIB / CF_DIBV5（最通用位图，浏览器常给 DIBV5）
  for(int t=0;t<2;t++){
    memset(&fe,0,sizeof(fe)); fe.cfFormat=(t==0)?CF_DIB:CF_DIBV5; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
    memset(&med,0,sizeof(med));
    if(pdo->GetData(&fe,&med)==S_OK){
      bool ok=false;
      if(med.hGlobal){ SIZE_T sz=GlobalSize(med.hGlobal); const void* p=GlobalLock(med.hGlobal);
        if(p){ std::vector<BYTE> dib((const BYTE*)p,(const BYTE*)p+sz); GlobalUnlock(med.hGlobal); ok=dibToARGB(dib,out,W,H); } }
      ReleaseStgMedium(&med);
      if(ok) return true;
    }
  }
  // 2) DragImageBits（浏览器/Shell 拖图：可能是 DIB / JPEG / PNG / SHDRAGIMAGE）
  if(g_cfDragImageBits){
    memset(&fe,0,sizeof(fe)); fe.cfFormat=g_cfDragImageBits; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
    memset(&med,0,sizeof(med));
    if(pdo->GetData(&fe,&med)==S_OK && med.hGlobal){
      SIZE_T sz=GlobalSize(med.hGlobal);
      const BYTE* data=(const BYTE*)GlobalLock(med.hGlobal);
      bool ok=false;
      if(data && sz>=4){
        DWORD head=0; memcpy(&head,data,4);
        if(head==0x28u || head==0x7Cu){                          // DIB biSize
          std::vector<BYTE> dib(data,data+sz);
          ok=dibToARGB(dib,out,W,H);
        } else if((data[0]==0xFF&&data[1]==0xD8)||(data[0]==0x89&&data[1]==0x50)){ // JPEG/PNG
          HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,sz);
          if(g){ void* p=GlobalLock(g); memcpy(p,data,sz); GlobalUnlock(g); IStream* st=nullptr;
            if(CreateStreamOnHGlobal(g,TRUE,&st)==S_OK){ Bitmap bmp(st); ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); st->Release(); } else GlobalFree(g); }
        } else {                                                 // SHDRAGIMAGE
          const SDragImage* sdi=(const SDragImage*)data;
          if(sdi->hbmp){ Bitmap bmp(sdi->hbmp,nullptr); ok=(bmp.GetLastStatus()==Gdiplus::Ok)&&argbFromBitmap(bmp,out,W,H); }
        }
      }
      GlobalUnlock(med.hGlobal);
      ReleaseStgMedium(&med);
      if(ok) return true;
    }
  }
  // 3) PNG 数据
  if(g_cfPNG){
    memset(&fe,0,sizeof(fe)); fe.cfFormat=g_cfPNG; fe.dwAspect=DVASPECT_CONTENT; fe.lindex=-1; fe.tymed=TYMED_HGLOBAL;
    memset(&med,0,sizeof(med));
    if(pdo->GetData(&fe,&med)==S_OK && med.hGlobal){
      SIZE_T sz=GlobalSize(med.hGlobal); const void* p=GlobalLock(med.hGlobal);
      if(p){ std::vector<BYTE> data; data.assign((const BYTE*)p,(const BYTE*)p+sz); GlobalUnlock(med.hGlobal);
        if(!data.empty() && decodePNG(data,out,W,H)){ ReleaseStgMedium(&med); return true; } }
      ReleaseStgMedium(&med);
    }
  }
  return false;
}
class TexelDropTarget : public IDropTarget {
public:
  TexelDropTarget():m_ref(1),m_accept(false){}
  STDMETHOD(QueryInterface)(REFIID riid,void** ppv){
    if(ppv==nullptr) return E_POINTER;
    if(riid==IID_IUnknown||riid==IID_IDropTarget){ *ppv=this; AddRef(); return S_OK; }
    *ppv=nullptr; return E_NOINTERFACE;
  }
  STDMETHOD_(ULONG,AddRef)(){ return ++m_ref; }
  STDMETHOD_(ULONG,Release)(){ ULONG r=--m_ref; if(r==0) delete this; return r; }
  STDMETHOD(DragEnter)(IDataObject* pdo,DWORD,POINTL,DWORD* pdwEffect){
    m_accept=(!g_setup && idfHasSupported(pdo));
    *pdwEffect=m_accept? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  STDMETHOD(DragOver)(DWORD,POINTL,DWORD* pdwEffect){
    *pdwEffect=m_accept? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  STDMETHOD(DragLeave)(){ return S_OK; }
  STDMETHOD(Drop)(IDataObject* pdo,DWORD,POINTL,DWORD* pdwEffect){
    *pdwEffect=DROPEFFECT_NONE;
    if(g_setup) return S_OK;
    RECT r=emptyRectPx();
    // 1) 文件优先（CF_HDROP）：.texel.zip / 图片 / 文本；浏览器拖图时 CF_HDROP 里的临时图优先于 URL 文本
    std::vector<std::wstring> fs;
    if(idfReadFiles(pdo,fs)){ for(auto& p:fs){ r=pasteDroppedFile(p.c_str()); if(!rectEmpty(r)) break; } }
    // 2) 虚拟文件图片（FileContents：浏览器拖 <img> 的原始字节）
    if(rectEmpty(r)){ std::vector<uint32_t> img; int W=0,H=0;
      for(int i=0;i<4;i++){ if(idfReadFileContentsImage(pdo,i,img,W,H)){ r=pasteImageARGB(img,W,H); break; } } }
    // 3) 位图（CF_DIB / DragImageBits / PNG）
    if(rectEmpty(r)){ std::vector<uint32_t> img; int W=0,H=0; if(idfReadImageARGB(pdo,img,W,H)) r=pasteImageARGB(img,W,H); }
    // 3) 文本
    if(rectEmpty(r)){ std::wstring t; if(idfReadText(pdo,t)) r=pasteText(t); }
    if(!rectEmpty(r)){ commitRect(r); overlayChanged(); *pdwEffect=DROPEFFECT_COPY; }
    return S_OK;
  }
private:
  ULONG m_ref;
  bool m_accept;
};

// ---------------- 配置 ----------------
static void configPath(wchar_t* out,int n){
  wchar_t p[MAX_PATH];
  DWORD r=GetModuleFileNameW(nullptr,p,MAX_PATH);
  if(r==0||r>=MAX_PATH) wcscpy(p,L".");
  wchar_t* s=wcsrchr(p,L'\\');
  if(s && s!=p) *(s+1)=0; else wcscpy(p,L".");
  swprintf(out,n,L"%ls\\texel.ini",p);
}
// 背景色：留空/trans/透明 = 透明(0x00FFFFFF)；否则 #RRGGBB 不透明
static bool bgIsTrans(){ return (g_bg>>24)==0; }
static bool parseBg(const wchar_t* s,uint32_t* out){
  const wchar_t* q=s; while(*q==L' '||*q==L'\t') q++;
  if(*q==0){ *out=0x00FFFFFFu; return true; }
  wchar_t low[16]; int n=0; for(; q[n]&&n<15; n++) low[n]=(wchar_t)towlower(q[n]); low[n]=0;
  if(wcscmp(low,L"trans")==0 || wcscmp(low,L"透明")==0){ *out=0x00FFFFFFu; return true; }
  uint32_t c; if(parseColor(s,&c)){ *out=0xFF000000u|c; return true; }
  return false;
}
static void bgToStr(wchar_t* b,int n){
  if(bgIsTrans()) wcscpy(b,L"trans"); else swprintf(b,n,L"%06X",(unsigned)(g_bg&0xFFFFFF));
}
static void loadConfig(){
  wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
  g_W=GetPrivateProfileIntW(L"cfg",L"W",50,p);
  g_H=GetPrivateProfileIntW(L"cfg",L"H",50,p);
  g_winX=GetPrivateProfileIntW(L"cfg",L"X",0,p);
  g_winY=GetPrivateProfileIntW(L"cfg",L"Y",0,p);
  g_hasPos=GetPrivateProfileIntW(L"cfg",L"Pos",0,p)!=0;
  g_noSavePrompt=GetPrivateProfileIntW(L"cfg",L"NoSavePrompt",0,p)!=0;
  { int wg=GetPrivateProfileIntW(L"cfg",L"WheelGain",1024,p); if(wg<1)wg=1; if(wg>4096)wg=4096; g_wheelGain=(double)wg; }
  wchar_t b[16]; uint32_t c;
  GetPrivateProfileStringW(L"cfg",L"Bg",L"trans",b,16,p); { uint32_t x; if(parseBg(b,&x)) g_bg=x; }
  GetPrivateProfileStringW(L"cfg",L"Fg",L"000000",b,16,p); if(parseColor(b,&c)) g_fg=c;
  if(g_W<2) g_W=2;
  if(g_H<1) g_H=1;
}
static void saveConfig(){
  wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
  wchar_t b[16];
  swprintf(b,16,L"%d",g_W); WritePrivateProfileStringW(L"cfg",L"W",b,p);
  swprintf(b,16,L"%d",g_H); WritePrivateProfileStringW(L"cfg",L"H",b,p);
  bgToStr(b,16); WritePrivateProfileStringW(L"cfg",L"Bg",b,p);
  swprintf(b,16,L"%06X",(unsigned)(g_fg&0xFFFFFF)); WritePrivateProfileStringW(L"cfg",L"Fg",b,p);
  WINDOWPLACEMENT wp; memset(&wp,0,sizeof(wp)); wp.length=sizeof(wp);
  if(g_hwnd && GetWindowPlacement(g_hwnd,&wp)){
    swprintf(b,16,L"%d",wp.rcNormalPosition.left); WritePrivateProfileStringW(L"cfg",L"X",b,p);
    swprintf(b,16,L"%d",wp.rcNormalPosition.top);  WritePrivateProfileStringW(L"cfg",L"Y",b,p);
    WritePrivateProfileStringW(L"cfg",L"Pos",L"1",p);
  }
}

// ---------------- 帮助 ----------------
static const wchar_t* HELP_TEXT =
L"字画 texel —— 极简像素草稿本\r\n"
L"\r\n"
L"【画布】\r\n"
L"  窗口始终可缩放；画布尺寸在『新建』时确定后锁定，随窗口居中。\r\n"
L"  宽单位 = 半角列(8px)；高单位 = 行(16px)；全角字占 2 列。\r\n"
L"  背景色 = 画布底色（留空/trans = 透明，导出 PNG 带透明通道；屏幕显示为白）。\r\n"
L"\r\n"
L"【图层】笔迹层在下，文字层在上（文字盖住笔迹）。\r\n"
L"\r\n"
L"【鼠标】\r\n"
L"  左键单击：定位文字光标；点底栏 = 切换历史。\r\n"
L"  左键长按/拖动：矩形选中，选中区反色显示。\r\n"
L"  Ctrl+左键点击/滑动：自由选中；Ctrl+右键点击/滑动：取消选中。\r\n"
L"  右键拖动：画笔；按住 Shift：八向直线。\r\n"
L"  滚轮：调整画笔粗细（慢拨 ±1，拨得越快步进越大）。\r\n"
L"\r\n"
L"【键盘】\r\n"
L"  直接打字：覆盖当前格，光标右移。\r\n"
L"  方向键：移动光标；Home / End：行首 / 行尾。\r\n"
L"  Enter：下一行行首；Backspace：删左边一个字。\r\n"
L"  Ctrl+A：全选；Ctrl+C：复制；Ctrl+X：剪切；Ctrl+V：粘贴（算一次『写』）。\r\n"
L"  选区含文字与笔迹；复制/剪切/粘贴连颜色与笔迹一起（粘贴为图章：透明处不动）。\r\n"
L"  从外部程序粘贴只有文字，一律用“当前”色。\r\n"
L"  也可粘贴图片(PNG/BMP/JPG… 或剪贴板图像)：以光标为左上角 1:1 贴上，超出画布忽略。\r\n"
L"  Ctrl+Z：回撤；Ctrl+Y：重做。\r\n"
L"  Ctrl+S：保存（PNG / .texel.zip，默认 .texel.zip）。\r\n"
L"\r\n"
L"【顶栏】\r\n"
L"  粗：画笔粗细 1–画布对角线（滚轮慢拨±1、越快越大）；颜色：当前画笔色 (#RRGGBB)。\r\n"
L"  色块：当前 / 前景 / 背景 / 透明 / 经典16色 / 历史。\r\n"
L"  改颜色后追加历史色块，栏满挤掉最旧（前、背景与常用色固定）。\r\n"
L"  打字与画笔都用“当前”色；每个字记住自己落笔时的颜色（透明=看得见字但无墨）。\r\n"
L"\r\n"
L"【底栏】历史时间轴，从左向右生长：写 = 文字，画 = 笔迹；\r\n"
L"  深色 = 已应用，灰色 = 已回撤；点击切换，满了挤掉最旧。\r\n"
L"\r\n"
L"【打开 / 拖放】\r\n"
L"  直接打开 texel.exe：进入设置模式，填宽/高/背景/前景后点『新建』。\r\n"
L"  把 .texel.zip 拖到 texel.exe 上：以该文件尺寸新开进程打开。\r\n"
L"  往画布里拖入文件或从外部程序拖入选区：.texel.zip 打开、图片贴图、文本按当前色写入。\r\n"
L"\r\n"
L"【配置 texel.ini（可手动编辑）】\r\n"
L"  NoSavePrompt=1  退出不提示保存，直接关闭\r\n"
L"  WheelGain=1024  滚轮加速封顶(与画布无关；越大越快，慢拨仍±1)\r\n"
L"\r\n"
L"  退出时若未保存，会提示保存。\r\n";
static void showHelp(){
  MessageBoxW(g_hwnd,HELP_TEXT,L"字画 · 帮助",MB_OK|MB_ICONINFORMATION);
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
  g_cols=W;
  g_pw=W*8; g_ph=H*16; g_cw=g_pw; g_ch=g_TB+g_ph+g_SB; g_ox=0; g_visW=g_pw; g_canvasY=g_TB; g_visH=g_ph;
  g_cells.assign((size_t)g_H*g_cols,0u);
  g_cellColor.assign((size_t)g_H*g_cols,0u);
  g_ink.assign((size_t)g_pw*g_ph,0u);
  g_text.assign((size_t)g_pw*g_ph,0u);
  g_fb.assign((size_t)g_pw*g_ph,0u);
  g_strokes.clear(); g_ops.clear(); g_pos=0; baseReset();
  g_maxSize=(int)std::sqrt((double)g_pw*g_pw+(double)g_ph*g_ph)+1;  // 一戳盖满画布
  g_cx=0; g_cy=0; g_size=1; g_brush=argb(fg); g_dirty=false;
  g_selecting=false; g_lbtnDown=false; g_ctrlSel=false; g_ctrlDesel=false; selCaret();
  g_sw.clear();
  g_sw.push_back({g_brush,0});
  g_sw.push_back({argb(fg),1});
  g_sw.push_back({argb(bg),2});
  g_sw.push_back({0u,3});
  for(int i=0;i<16;i++) g_sw.push_back({argb(EGA16[i]),4});
  createDIB();
  RECT rc={0,0,g_cw,g_ch};
  AdjustWindowRectEx(&rc, GetWindowLongW(g_hwnd,GWL_STYLE), FALSE, 0);
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

// ---------------- 设置模式（无文件启动） ----------------
static bool g_setupBusy=false;
static HWND g_hSW=nullptr,g_hSH=nullptr,g_hSBG=nullptr,g_hSFG=nullptr,g_hSOK=nullptr,g_hSOPEN=nullptr;
enum { IDC_SW=2101, IDC_SH=2102, IDC_SBG=2103, IDC_SFG=2104, IDC_SOK=2105, IDC_SOPEN=2106 };
static void setupConfirm();
static void setupLayout();
static LRESULT CALLBACK SetupEditProc(HWND h,UINT m,WPARAM w,LPARAM l){
  if(m==WM_KEYDOWN && w==VK_RETURN){ if(g_setup) setupConfirm(); return 0; }
  if(m==WM_CHAR && w==13) return 0;
  return CallWindowProc(g_editProc,h,m,w,l);
}
static void applyCanvasSize(int W,int H){
  g_W=W; g_H=H; g_cols=W; g_pw=W*8; g_ph=H*16;
  g_cells.assign((size_t)g_H*g_cols,0u);
  g_cellColor.assign((size_t)g_H*g_cols,0u);
  g_ink.assign((size_t)g_pw*g_ph,0u);
  g_text.assign((size_t)g_pw*g_ph,0u);
  g_fb.assign((size_t)g_pw*g_ph,0u);
  g_strokes.clear(); g_ops.clear(); g_pos=0; baseReset();
  g_maxSize=(int)std::sqrt((double)g_pw*g_pw+(double)g_ph*g_ph)+1;
  selClear();
  renderText(); compose();
}
static void computeLayout(){
  g_ox=(g_cw-g_pw)/2; if(g_ox<0)g_ox=0;
  g_visW=g_pw; if(g_ox+g_visW>g_cw) g_visW=g_cw-g_ox; if(g_visW<0)g_visW=0;
  int aT=g_TB, aB=g_ch-g_SB; if(aB<aT) aB=aT;
  g_canvasY=aT+(aB-aT-g_ph)/2; if(g_canvasY<aT) g_canvasY=aT;   // 纵向居中
  g_visH=g_ph; if(g_canvasY+g_visH>aB) g_visH=aB-g_canvasY; if(g_visH<0)g_visH=0;
}
static void placeTopEdits(){
  if(!g_hEditSize||!g_hEdit) return;
  MoveWindow(g_hEditSize, g_editSizeX, (g_TB-16)/2, g_editSizeW, 16, TRUE);
  MoveWindow(g_hEdit, g_colorX, (g_TB-16)/2, g_colorW, 16, TRUE);
}
static void setFieldTexts(int W,int H){
  if(g_setupBusy||!g_hSW) return;
  wchar_t b[16]; swprintf(b,16,L"%d",W); SetWindowTextW(g_hSW,b);
  swprintf(b,16,L"%d",H); SetWindowTextW(g_hSH,b);
}
static void setupApplySize(){
  if(g_setupBusy) return;
  g_setupBusy=true;
  int Wmax=g_cw/8; if(Wmax<2)Wmax=2;                   // 上限 = 当前窗口可容纳的最大列数
  int Hmax=(g_ch-g_TB-g_SB)/16; if(Hmax<1)Hmax=1;
  wchar_t b[32];
  GetWindowTextW(g_hSW,b,32); int W=_wtoi(b); if(W<2)W=2; if(W>Wmax)W=Wmax;
  GetWindowTextW(g_hSH,b,32); int H=_wtoi(b); if(H<1)H=1; if(H>Hmax)H=Hmax;
  applyCanvasSize(W,H);                       // 只改画布，不改窗口
  computeLayout();
  createDIB(); placeTopEdits(); setupLayout();
  swprintf(b,32,L"%d",W); SetWindowTextW(g_hSW,b);      // 回填实际(已夹)值
  swprintf(b,32,L"%d",H); SetWindowTextW(g_hSH,b);
  InvalidateRect(g_hwnd,nullptr,FALSE);
  g_setupBusy=false;
}
static void setupLayout(){
  if(!g_hSW) return;
  int cx=g_cw/2, cy=g_ch/2;                 // 悬浮于窗口中央，脱离画布
  int left=cx-95, top=cy-(4*24+36)/2;
  MoveWindow(g_hSW, left+66, top+2,  110,20, TRUE);
  MoveWindow(g_hSH, left+66, top+26, 110,20, TRUE);
  MoveWindow(g_hSBG,left+66, top+50, 110,20, TRUE);
  MoveWindow(g_hSFG,left+66, top+74, 110,20, TRUE);
  MoveWindow(g_hSOPEN, cx-54, top+4*24+6, 50,22, TRUE);
  MoveWindow(g_hSOK, cx+4, top+4*24+6, 50,22, TRUE);
}
static void setupCreateControls(){
  g_setupBusy=true;
  DWORD st=WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL;
  g_hSW =CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",st|ES_NUMBER,0,0,10,10,g_hwnd,(HMENU)IDC_SW,g_hInst,nullptr);
  g_hSH =CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",st|ES_NUMBER,0,0,10,10,g_hwnd,(HMENU)IDC_SH,g_hInst,nullptr);
  g_hSBG=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",st,0,0,10,10,g_hwnd,(HMENU)IDC_SBG,g_hInst,nullptr);
  g_hSFG=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",st,0,0,10,10,g_hwnd,(HMENU)IDC_SFG,g_hInst,nullptr);
  g_hSOPEN=CreateWindowExW(0,L"BUTTON",L"打开",WS_CHILD|WS_VISIBLE,0,0,10,10,g_hwnd,(HMENU)IDC_SOPEN,g_hInst,nullptr);
  g_hSOK=CreateWindowExW(0,L"BUTTON",L"新建",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,0,0,10,10,g_hwnd,(HMENU)IDC_SOK,g_hInst,nullptr);
  HWND es[4]={g_hSW,g_hSH,g_hSBG,g_hSFG};
  for(int i=0;i<4;i++){ SendMessageW(es[i],WM_SETFONT,(WPARAM)g_uiFont,TRUE); SetWindowLongPtrW(es[i],GWLP_WNDPROC,(LONG_PTR)SetupEditProc); }
  SendMessageW(g_hSOK,WM_SETFONT,(WPARAM)g_uiFont,TRUE);
  SendMessageW(g_hSOPEN,WM_SETFONT,(WPARAM)g_uiFont,TRUE);
  wchar_t b[32];
  swprintf(b,32,L"%d",g_W); SetWindowTextW(g_hSW,b);
  swprintf(b,32,L"%d",g_H); SetWindowTextW(g_hSH,b);
  if((g_bg>>24)==0) wcscpy(b,L"trans"); else swprintf(b,32,L"%06X",(unsigned)(g_bg&0xFFFFFF)); SetWindowTextW(g_hSBG,b);
  swprintf(b,32,L"%06X",(unsigned)(g_fg&0xFFFFFF)); SetWindowTextW(g_hSFG,b);
  setupLayout();
  g_setupBusy=false;
}
static void setupOpen(){
  wchar_t path[MAX_PATH]=L"";
  OPENFILENAMEW ofn; memset(&ofn,0,sizeof(ofn)); ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=g_hwnd;
  ofn.lpstrFilter=L"texel (*.texel.zip)\0*.texel.zip\0All\0*.*\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
  ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER;
  if(!GetOpenFileNameW(&ofn)) return;
  HWND cs[6]={g_hSW,g_hSH,g_hSBG,g_hSFG,g_hSOK,g_hSOPEN};
  for(int i=0;i<6;i++) if(cs[i]) DestroyWindow(cs[i]);
  g_hSW=g_hSH=g_hSBG=g_hSFG=g_hSOK=g_hSOPEN=nullptr;
  g_setup=false;
  openTexelZipFile(path);
  SetFocus(g_hwnd);
}
static void setupConfirm(){
  wchar_t b[32]; uint32_t c;
  GetWindowTextW(g_hSBG,b,32); if(!parseBg(b,&c)) c=g_bg;
  GetWindowTextW(g_hSFG,b,32); uint32_t f; if(!parseColor(b,&f)) f=g_fg; f=argb(f);
  HWND cs[6]={g_hSW,g_hSH,g_hSBG,g_hSFG,g_hSOK,g_hSOPEN};
  for(int i=0;i<6;i++) if(cs[i]) DestroyWindow(cs[i]);
  g_hSW=g_hSH=g_hSBG=g_hSFG=g_hSOK=g_hSOPEN=nullptr;
  g_setup=false;
  g_bg=c; g_fg=f; g_brush=argb(f); g_dirty=false;   // 不改变窗口/画布尺寸，只锁 + 收色
  g_sw[0].color=g_brush; g_sw[1].color=argb(f); g_sw[2].color=argb(c);
  compose();                                        // 背景色变了，重新合成整帧
  refreshEdit(); refreshSize();
  SetFocus(g_hwnd);
  InvalidateRect(g_hwnd,nullptr,FALSE);
}
static void setupDrawLabels(){
  if(!g_hSW) return;
  int cx=g_cw/2, cy=g_ch/2;
  int left=cx-95, top=cy-(4*24+36)/2;
  const wchar_t* lb[4]={L"宽 (半角):",L"高 (行):",L"背景色:",L"前景色:"};
  HGDIOBJ oldF=SelectObject(g_memDC,g_uiFont);
  int oldBk=SetBkMode(g_memDC,TRANSPARENT);
  COLORREF oldTx=SetTextColor(g_memDC,RGB(0x20,0x20,0x20));
  for(int i=0;i<4;i++){ RECT r={left, top+i*24+4, left+62, top+i*24+20}; DrawTextW(g_memDC,lb[i],-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE); }
  SetTextColor(g_memDC,oldTx); SetBkMode(g_memDC,oldBk); SelectObject(g_memDC,oldF);
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
    case WM_ERASEBKGND: return 1;

    case WM_SIZE:{
      if(g_memDC){
        int cw=LOWORD(lp), chh=HIWORD(lp);
        if(cw>0&&chh>0){
          g_cw=cw; g_ch=chh;
          if(g_setup){
            int W=cw/8; if(W<2)W=2;
            int H=(chh-g_TB-g_SB)/16; if(H<1)H=1;
            applyCanvasSize(W,H);
            setFieldTexts(W,H);
          }
          computeLayout();                        // 水平居中 + 可见宽度
          createDIB(); placeTopEdits();
          if(g_setup) setupLayout();
          InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
      }
      break;
    }
    case WM_GETMINMAXINFO:{
      MINMAXINFO* mm=(MINMAXINFO*)lp;
      RECT rc={0,0,g_minWinW,g_minWinH};                              // 最小：宽=默认色块全显示，高=设置控件全显示
      AdjustWindowRectEx(&rc, GetWindowLongW(hwnd,GWL_STYLE), FALSE, 0);
      mm->ptMinTrackSize.x=rc.right-rc.left;
      mm->ptMinTrackSize.y=rc.bottom-rc.top;
      RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);        // 最大：不超出屏幕工作区
      mm->ptMaxTrackSize.x=wa.right-wa.left;
      mm->ptMaxTrackSize.y=wa.bottom-wa.top;
      return 0;
    }
    case WM_COMMAND:{
      int id=LOWORD(wp), code=HIWORD(wp);
      if(g_setup){
        if(code==EN_CHANGE && (id==IDC_SW||id==IDC_SH)){ setupApplySize(); return 0; }
        if(code==BN_CLICKED && id==IDC_SOK){ setupConfirm(); return 0; }
        if(code==BN_CLICKED && id==IDC_SOPEN){ setupOpen(); return 0; }
      }
      return 0;
    }

    case WM_PAINT:{
      PAINTSTRUCT ps;
      HDC dc=BeginPaint(hwnd,&ps);
      RECT R=ps.rcPaint;
      for(int yy=0;yy<g_ch;yy++){ uint32_t* pp=g_memBits+(size_t)yy*g_cw; for(int xx=0;xx<g_cw;xx++) pp[xx]=0xFFF0F0F0u; } // 留白
      drawTopBar();
      drawStatusBar();
      for(int y=0;y<g_visH;y++)         // 画布（水平/纵向居中，超出裁剪）
        memcpy(g_memBits+(size_t)(g_canvasY+y)*g_cw+g_ox, g_fb.data()+(size_t)y*g_pw, (size_t)g_visW*4);
      if(g_setup) setupDrawLabels();
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
      if(g_setup && y>=g_TB) return 0;
      if(y<g_TB){
        if(x>=HELP_X && x<HELP_X+HELP_W && y>=HELP_Y && y<HELP_Y+HELP_H){ showHelp(); return 0; }
        if(x>=0 && x<g_cw) clickSwatch(x,y);
        return 0;
      }
      if(y>=g_ch-g_SB){
        if(x>=0 && x<STATUS_COORD_W){ setPos(0); return 0; }   // 点坐标：操作历史回到初始状态（撤销全部）
        if(x>=STATUS_COORD_W && x<g_cw){ int i=(x-STATUS_COORD_W)/16; if(i>=0&&i<(int)g_ops.size()) setPos(i+1);} return 0;
      }
      int cy=y-g_canvasY;
      int col=(x-g_ox)/COLW, row=cy/ROWH;
      col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
      if(g_cells[(size_t)row*g_cols+col]==CONT) col--;
      if(GetKeyState(VK_CONTROL)&0x8000){                       // Ctrl+左键：点选/滑选选中
        g_lbtnDown=true; g_ctrlSel=true;
        g_selDragC=col; g_selDragR=row;
        selSet(col,row,true);
        overlayChanged();
        return 0;
      }
      g_cx=col; g_cy=row;           // 按下即定位光标（点击/长按统一）
      g_lbtnDown=true; g_selecting=false;
      g_pressPt.x=x; g_pressPt.y=y;
      g_selAnchorC=col; g_selAnchorR=row;
      selCaret();
      SetTimer(hwnd,2,400,nullptr);
      overlayChanged();
      return 0;
    }
    case WM_MOUSEMOVE:{
      int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
      int cx=x-g_ox;
      if(g_setup) return 0;
      if(g_drawing){
        int cy=y-g_canvasY;
        Stroke& st=g_strokes.back();
        int sx=st.pts.back().x, sy=st.pts.back().y;
        if(GetKeyState(VK_SHIFT)&0x8000){
          int ax=st.pts[0].x, ay=st.pts[0].y;
          POINT e=snap8(ax,ay,cx,cy);
          st.pts.clear(); st.pts.push_back({ax,ay}); st.pts.push_back(e);
          rebuildInk(); redrawAllCanvas();
        } else {
          lineBuf(g_ink.data(),sx,sy,cx,cy,g_size/2,g_brush);
          st.pts.push_back({cx,cy});
          commitRect(brushRectPx(sx,sy,cx,cy,g_size/2));
        }
        g_last.x=cx; g_last.y=cy;
        return 0;
      }
      if(g_ctrlSel){                                     // Ctrl+左键滑选
        int col=cx/COLW, row=(y-g_canvasY)/ROWH;
        col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
        selLine(g_selDragC,g_selDragR,col,row,true);
        g_selDragC=col; g_selDragR=row;
        overlayChanged();
        return 0;
      }
      if(g_ctrlDesel){                                   // Ctrl+右键滑取消
        int col=cx/COLW, row=(y-g_canvasY)/ROWH;
        col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
        selLine(g_selDragC,g_selDragR,col,row,false);
        g_selDragC=col; g_selDragR=row;
        overlayChanged();
        return 0;
      }
      if(g_lbtnDown){
        if(!g_selecting){
          int dx=x-g_pressPt.x, dy=y-g_pressPt.y;
          if(dx*dx+dy*dy>64){ g_selecting=true; KillTimer(hwnd,2); }
        }
        if(g_selecting){
          int col=cx/COLW, row=(y-g_canvasY)/ROWH;
          col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
          selRect(g_selAnchorC,col,g_selAnchorR,row);
          overlayChanged();
        }
        return 0;
      }
      return 0;
    }
    case WM_LBUTTONUP:{
      KillTimer(hwnd,2);
      if(g_ctrlSel){ g_ctrlSel=false; g_lbtnDown=false; return 0; }
      if(g_lbtnDown){
        if(g_selecting){ g_selecting=false; }
        g_lbtnDown=false;
        overlayChanged();
      }
      return 0;
    }
    case WM_RBUTTONDOWN:{
      int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
      if(g_setup) return 0;
      if(y<g_TB||y>=g_ch-g_SB) return 0;
      int cx=x-g_ox; if(cx<0||cx>=g_pw) return 0;
      int cy=y-g_canvasY;
      if(GetKeyState(VK_CONTROL)&0x8000){                  // Ctrl+右键：点选/滑选取消
        int col=cx/COLW, row=cy/ROWH;
        col=imin(imax(col,0),g_cols-1); row=imin(imax(row,0),g_H-1);
        g_ctrlDesel=true; SetCapture(hwnd);
        g_selDragC=col; g_selDragR=row;
        selSet(col,row,false);
        overlayChanged();
        return 0;
      }
      selCaret();
      truncateFuture();
      g_drawing=true; SetCapture(hwnd);
      g_last.x=cx; g_last.y=cy;
      Stroke st; st.color=g_brush; st.size=g_size; st.pts.push_back({cx,cy});
      g_strokes.push_back(st);
      Op op; op.type=OP_STROKE; op.color=g_brush; op.size=g_size;
      op.pts.push_back({cx,cy}); op.cbx1=g_cx; op.cby1=g_cy;
      pushOp(op);
      stampBuf(g_ink.data(),cx,cy,g_size/2,g_brush);
      g_dirty=true;
      overlayChanged();
      commitRect(brushRectPx(cx,cy,cx,cy,g_size/2));
      return 0;
    }
    case WM_RBUTTONUP:{
      if(g_ctrlDesel){ g_ctrlDesel=false; ReleaseCapture(); return 0; }
      if(g_drawing){
        g_drawing=false; ReleaseCapture();
        if(!g_ops.empty()) g_ops.back().pts=g_strokes.back().pts;
      }
      return 0;
    }
    case WM_DROPFILES:{
      if(g_setup){ DragFinish((HDROP)wp); return 0; }   // 新建前拖动不生效
      std::vector<std::wstring> fs=hdropFiles((HANDLE)wp);
      RECT r=emptyRectPx();
      for(auto& p:fs){ r=pasteDroppedFile(p.c_str()); if(!rectEmpty(r)) break; }
      DragFinish((HDROP)wp);
      commitRect(r); overlayChanged();
      return 0;
    }
    case WM_CONTEXTMENU: return 0;

    case WM_MOUSEWHEEL:{
      int dz=GET_WHEEL_DELTA_WPARAM(wp);
      if(g_setup) return 0;
      DWORD now=GetTickCount();
      DWORD dt=now-g_wheelT; g_wheelT=now;            // 距上次滚轮的间隔(ms)
      if(dt>1000) dt=1000; if(dt<1) dt=1;
      if(g_wheelEma<=0.0) g_wheelEma=(double)dt; else g_wheelEma=0.6*g_wheelEma+0.4*(double)dt; // 平滑测速
      const double T_SLOW=250.0, T_FAST=15.0;         // 人类灵敏度边界(ms)
      double inv=1.0/g_wheelEma, invS=1.0/T_SLOW, invF=1.0/T_FAST;
      double u=(inv-invS)/(invF-invS); if(u<0)u=0; if(u>1)u=1;   // 速度档 0慢..1极快
      const double Gm=g_wheelGain;                    // 增益上限(ini: WheelGain，与画布无关)
      double gain=1.0+(Gm-1.0)*u*u;                   // 死区(=1) + 封顶增益曲线(凸2)
      int step=(int)(gain+0.5); if(step<1)step=1;
      if(dz>0) g_size+=step; else if(dz<0) g_size-=step;
      if(g_size<1) g_size=1; if(g_size>g_maxSize) g_size=g_maxSize;
      refreshSize();
      return 0;
    }

    case WM_TIMER:{
      if(wp==2 && g_lbtnDown && !g_selecting && !g_ctrlSel){
        g_selecting=true; KillTimer(hwnd,2);
        overlayChanged();
      }
      return 0;
    }

    case WM_KEYDOWN:{
      if(g_setup) return 0;
      if(imeComposing(hwnd)) return DefWindowProc(hwnd,msg,wp,lp);
      if(GetKeyState(VK_CONTROL)&0x8000){
        if(wp=='Z'){ doUndo(); return 0; }
        if(wp=='Y'){ setPos(g_pos+1); return 0; }
        if(wp=='S'){ saveFile(); return 0; }
        if(wp=='C'){ doCopy(); return 0; }
        if(wp=='X'){ RECT r=doCut(); commitRect(r); overlayChanged(); return 0; }
        if(wp=='A'){ selRect(0,g_cols-1,0,g_H-1); overlayChanged(); return 0; }
        if(wp=='V'){ RECT r=doPaste(); commitRect(r); overlayChanged(); return 0; }
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
      if(moved){ selCaret(); overlayChanged(); }
      return 0;
    }
    case WM_CHAR:{
      if(g_setup) return 0;
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
        if(r==1){ if(!saveFile()) return 0; }
        else if(r==3){
          wchar_t p[MAX_PATH]; configPath(p,MAX_PATH);
          WritePrivateProfileStringW(L"cfg",L"NoSavePrompt",L"1",p);
          g_noSavePrompt=true;
        }
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY: RevokeDragDrop(hwnd); PostQuitMessage(0); return 0;
  }
  return DefWindowProc(hwnd,msg,wp,lp);
}

// ---------------- 入口 ----------------
int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int){
  g_hInst=hInst;
  SetProcessDPIAware();
  OleInitialize(nullptr);
  GdiplusStartupInput gi; ULONG_PTR tok=0;
  GdiplusStartup(&tok,&gi,nullptr);
  g_cfCells=RegisterClipboardFormatW(L"texel-cellblock");
  g_cfPNG=RegisterClipboardFormatW(L"PNG");
  g_cfDragImageBits=RegisterClipboardFormatW(L"DragImageBits");
  g_cfFileContents=RegisterClipboardFormatW(L"FileContents");

  loadConfig();
  std::wstring startFile;
  { int ac=0; LPWSTR* av=CommandLineToArgvW(GetCommandLineW(),&ac); if(av){ if(ac>=2) startFile=av[1]; LocalFree(av); } }
  bool setupMode=startFile.empty();

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

  g_cols=g_W;
  g_pw=g_W*8; g_ph=g_H*16;
  g_cw=g_pw; g_ch=g_TB+g_ph+g_SB;
  {   // 窗口不超屏幕；画布随之 clamp
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    if(g_cw>wa.right-wa.left) g_cw=wa.right-wa.left;
    if(g_ch>wa.bottom-wa.top) g_ch=wa.bottom-wa.top;
    if(g_cw<g_pw){ g_W=g_cw/8; if(g_W<2)g_W=2; g_cols=g_W; g_pw=g_W*8; }
    if(g_ch<g_TB+g_ph+g_SB){ g_H=(g_ch-g_TB-g_SB)/16; if(g_H<1)g_H=1; g_ph=g_H*16; }
  }

  DWORD wstyle=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN|WS_THICKFRAME|WS_MAXIMIZEBOX;
  RECT rc={0,0,g_cw,g_ch};
  AdjustWindowRectEx(&rc, wstyle, FALSE, 0);
  g_hwnd=CreateWindowExW(0,L"TexelClass",L"字画", wstyle,
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

  // 输入框宽度按等宽字体实测：色框留 8 字余量(6位hex+2，避免输入时滚动/看不全)，粗细框留 5 位
  int sizeX,sizeW,colorX,colorW;
  { HDC mdc=CreateCompatibleDC(nullptr); HGDIOBJ of=SelectObject(mdc,g_uiFont);
    SIZE sz; GetTextExtentPoint32W(mdc,L"000000",6,&sz);
    int cw=(sz.cx+5)/6, edge=GetSystemMetrics(SM_CXEDGE);   // cw=单字符宽
    sizeW =cw*5+edge*2+6;   // 最大可达画布对角线(最多5位)
    colorW=cw*8+edge*2+6;
    SelectObject(mdc,of); DeleteDC(mdc); }
  sizeX=40; colorX=sizeX+sizeW+6; g_swatchX0=colorX+colorW+8;
  g_editSizeX=sizeX; g_editSizeW=sizeW; g_colorX=colorX; g_colorW=colorW;
  g_minWinW=g_swatchX0 + 20*g_swPitch + 8;   // 固定色块(当前/前景/背景/透明/经典16)全显示
  g_minWinH=g_TB + 132 + g_SB;                // 顶栏 + 设置控件(4字段+按钮) + 底栏
  {   // 窗口初始若小于最小下界，扩到最小（画布保持按 ini，居中留白）
    RECT cr; GetClientRect(g_hwnd,&cr);
    if(cr.right-cr.left < g_minWinW || cr.bottom-cr.top < g_minWinH){
      int nw=imax(cr.right-cr.left, g_minWinW), nh=imax(cr.bottom-cr.top, g_minWinH);
      RECT wr={0,0,nw,nh};
      AdjustWindowRectEx(&wr, GetWindowLongW(g_hwnd,GWL_STYLE), FALSE, 0);
      SetWindowPos(g_hwnd,nullptr,0,0,wr.right-wr.left,wr.bottom-wr.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
      RECT cr2; GetClientRect(g_hwnd,&cr2);
      g_cw=cr2.right-cr2.left; g_ch=cr2.bottom-cr2.top;
    }
  }

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
  if(setupMode){ g_setup=true; setupCreateControls(); }
  else if(!startFile.empty()) openTexelZipFile(startFile.c_str());
  placeWindow();

  DragAcceptFiles(g_hwnd,TRUE);
  { TexelDropTarget* dt=new TexelDropTarget(); RegisterDragDrop(g_hwnd,dt); dt->Release(); }
  ShowWindow(g_hwnd,SW_SHOW);
  UpdateWindow(g_hwnd);
  SetFocus(g_hwnd);

  MSG msg;
  while(GetMessageW(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }

  saveConfig();
  GdiplusShutdown(tok);
  OleUninitialize();
  return 0;
}
