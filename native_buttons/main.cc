#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "common/stb/stb_truetype.h"

struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const {
        return px >= x && px < x+w && py >= y && py < y+h;
    }
};

struct App {
    ANativeActivity* activity;
    ANativeWindow* window;
    AInputQueue* input;
    ARect content;
    stbtt_fontinfo font;
    unsigned char* fontData;
    int count, pressed;
    float scale;
    Rect add, reset;
};

static uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

struct Canvas {
    ANativeWindow_Buffer b;
    void pixel(int x, int y, uint32_t color, unsigned alpha=255) {
        if (x < 0 || y < 0 || x >= b.width || y >= b.height) return;
        uint32_t& p = static_cast<uint32_t*>(b.bits)[y*b.stride+x];
        if (alpha == 255) { p=color; return; }
        unsigned r=((color&255)*alpha+(p&255)*(255-alpha))/255;
        unsigned g=(((color>>8)&255)*alpha+((p>>8)&255)*(255-alpha))/255;
        unsigned bl=(((color>>16)&255)*alpha+((p>>16)&255)*(255-alpha))/255;
        p=rgb(r,g,bl);
    }
    void fill(uint32_t color) {
        for(int y=0;y<b.height;++y)
            for(int x=0;x<b.width;++x) pixel(x,y,color);
    }
    void rounded(Rect r, float radius, uint32_t color) {
        int left=(int)r.x, top=(int)r.y;
        for(int y=top;y<(int)(r.y+r.h);++y) {
            for(int x=left;x<(int)(r.x+r.w);++x) {
                float dx=fmaxf(fmaxf(r.x+radius-x, x-(r.x+r.w-radius)),0);
                float dy=fmaxf(fmaxf(r.y+radius-y, y-(r.y+r.h-radius)),0);
                float edge=radius-sqrtf(dx*dx+dy*dy);
                if(edge>=0) pixel(x,y,color,(unsigned)(fminf(edge+0.5f,1)*255));
            }
        }
    }
    void text(App* app, const char* str, float cx, float baseline, float size, uint32_t color) {
        if(!app->fontData) return;
        float scale=stbtt_ScaleForPixelHeight(&app->font,size), width=0;
        for(const char* c=str;*c;++c) {
            int advance; stbtt_GetCodepointHMetrics(&app->font,*c,&advance,nullptr);
            width+=advance*scale;
            if(c[1]) width+=stbtt_GetCodepointKernAdvance(&app->font,*c,c[1])*scale;
        }
        float x=cx-width/2;
        for(const char* c=str;*c;++c) {
            int w,h,xoff,yoff,advance;
            unsigned char* pixels=stbtt_GetCodepointBitmap(&app->font,0,scale,*c,&w,&h,&xoff,&yoff);
            if(pixels) {
                for(int j=0;j<h;++j) for(int i=0;i<w;++i)
                    pixel((int)x+xoff+i,(int)baseline+yoff+j,color,pixels[j*w+i]);
                stbtt_FreeBitmap(pixels,nullptr);
            }
            stbtt_GetCodepointHMetrics(&app->font,*c,&advance,nullptr);
            x+=advance*scale;
            if(c[1]) x+=stbtt_GetCodepointKernAdvance(&app->font,*c,c[1])*scale;
        }
    }
};

static void save(App* app) {
    char path[1024];
    snprintf(path,sizeof(path),"%s/count.txt",app->activity->internalDataPath);
    FILE* file=fopen(path,"w");
    if(file) { fprintf(file,"%d\n",app->count); fclose(file); }
}

static void draw(App* app) {
    if(!app->window) return;
    Canvas c{};
    if(ANativeWindow_lock(app->window,&c.b,nullptr)!=0) return;
    const uint32_t ink=rgb(235,241,252), muted=rgb(153,170,194);
    c.fill(rgb(14,22,37));
    float left=0,top=0,right=(float)c.b.width,bottom=(float)c.b.height;
    if(app->content.right>app->content.left && app->content.bottom>app->content.top) {
        left=fmaxf(left,(float)app->content.left);
        top=fmaxf(top,(float)app->content.top);
        right=fminf(right,(float)app->content.right);
        bottom=fminf(bottom,(float)app->content.bottom);
    }
    float s=fminf((right-left)/400.0f,(bottom-top)/600.0f);
    if(s<=0) { ANativeWindow_unlockAndPost(app->window); return; }
    app->scale=s;
    float x=(left+right)/2, y=(top+bottom)/2-250*s;
    c.rounded({x-40*s,y,80*s,28*s},14*s,rgb(31,57,63));
    c.text(app,"HELLO",x,y+19*s,16*s,rgb(111,230,193));
    c.text(app,"native_buttons",x,y+82*s,38*s,ink);
    c.text(app,"A little counter. Give it a tap.",x,y+116*s,19*s,muted);
    c.rounded({x-164*s,y+151*s,328*s,178*s},24*s,rgb(24,36,55));
    c.text(app,"YOUR COUNT",x,y+190*s,15*s,muted);
    char number[24]; snprintf(number,sizeof(number),"%d",app->count);
    c.text(app,number,x,y+288*s,app->count>9999?78*s:102*s,ink);
    app->add={x-164*s,y+353*s,328*s,62*s};
    app->reset={x-164*s,y+431*s,328*s,62*s};
    c.rounded(app->add,18*s,app->pressed==1?rgb(58,185,152):rgb(109,231,193));
    c.rounded(app->reset,18*s,app->pressed==2?rgb(52,69,94):rgb(33,48,70));
    c.text(app,"+  Add one",x,y+392*s,25*s,rgb(12,43,36));
    c.text(app,"Reset",x,y+470*s,25*s,ink);
    c.text(app,"Saved automatically",x,y+530*s,16*s,muted);
    ANativeWindow_unlockAndPost(app->window);
}

static int inputReady(int, int, void* data) {
    App* app=static_cast<App*>(data);
    AInputEvent* event=nullptr;
    while(app->input && AInputQueue_getEvent(app->input,&event)>=0) {
        if(AInputQueue_preDispatchEvent(app->input,event)) continue;
        int handled=0;
        if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_MOTION) {
            int action=AMotionEvent_getAction(event)&AMOTION_EVENT_ACTION_MASK;
            float x=AMotionEvent_getX(event,0),y=AMotionEvent_getY(event,0);
            int hit=app->add.contains(x,y)?1:app->reset.contains(x,y)?2:0;
            if(action==AMOTION_EVENT_ACTION_DOWN) app->pressed=hit;
            else if(action==AMOTION_EVENT_ACTION_UP) {
                if(app->pressed && hit==app->pressed) {
                    if(hit==1 && app->count<999999) ++app->count;
                    if(hit==2) app->count=0;
                    save(app);
                }
                app->pressed=0;
            } else if(action==AMOTION_EVENT_ACTION_CANCEL || action==AMOTION_EVENT_ACTION_POINTER_DOWN
                      || (action==AMOTION_EVENT_ACTION_MOVE && hit!=app->pressed)) app->pressed=0;
            handled=1;
            draw(app);
        }
        AInputQueue_finishEvent(app->input,event,handled);
    }
    return 1;
}

static App* state(ANativeActivity* a) { return static_cast<App*>(a->instance); }
static void windowCreated(ANativeActivity* a, ANativeWindow* window) {
    state(a)->window=window;
    ANativeWindow_setBuffersGeometry(window,0,0,WINDOW_FORMAT_RGBA_8888);
    draw(state(a));
}
static void windowDestroyed(ANativeActivity* a, ANativeWindow*) { state(a)->window=nullptr; }
static void windowRedraw(ANativeActivity* a, ANativeWindow*) { draw(state(a)); }
static void inputCreated(ANativeActivity* a,AInputQueue* input) {
    state(a)->input=input;
    AInputQueue_attachLooper(input,ALooper_forThread(),ALOOPER_POLL_CALLBACK,inputReady,state(a));
}
static void inputDestroyed(ANativeActivity* a,AInputQueue* input) {
    AInputQueue_detachLooper(input); state(a)->input=nullptr; state(a)->pressed=0;
}
static void contentChanged(ANativeActivity* a,const ARect* rect) {
    state(a)->content=*rect; draw(state(a));
}
static void pauseApp(ANativeActivity* a) { state(a)->pressed=0; save(state(a)); }
static void resumeApp(ANativeActivity* a) { draw(state(a)); }
static void destroy(ANativeActivity* a) {
    App* app=state(a); save(app); free(app->fontData); free(app); a->instance=nullptr;
}
static void* saveState(ANativeActivity* a,size_t* size) {
    int* count=static_cast<int*>(malloc(sizeof(int)));
    if(!count) { *size=0; return nullptr; }
    *count=state(a)->count; *size=sizeof(int); return count;
}

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity,void* saved,size_t savedSize) {
    App* app=static_cast<App*>(calloc(1,sizeof(App)));
    if(!app) { ANativeActivity_finish(activity); return; }
    activity->instance=app; app->activity=activity;
    const char* fonts[]={"/system/fonts/RobotoStatic-Regular.ttf","/system/fonts/Roboto-Regular.ttf"};
    for(const char* path:fonts) {
        FILE* file=fopen(path,"rb"); if(!file) continue;
        fseek(file,0,SEEK_END); long length=ftell(file); rewind(file);
        if(length>0 && length<16000000) {
            app->fontData=static_cast<unsigned char*>(malloc(length));
            if(app->fontData && fread(app->fontData,1,length,file)==(size_t)length
               && stbtt_InitFont(&app->font,app->fontData,stbtt_GetFontOffsetForIndex(app->fontData,0))) {
                fclose(file); break;
            }
            free(app->fontData); app->fontData=nullptr;
        }
        fclose(file);
    }
    if(!app->fontData) {
        __android_log_print(ANDROID_LOG_ERROR,"native_buttons","No readable system font");
        free(app); activity->instance=nullptr; ANativeActivity_finish(activity); return;
    }
    char path[1024]; snprintf(path,sizeof(path),"%s/count.txt",activity->internalDataPath);
    FILE* file=fopen(path,"r");
    if(file) { if(fscanf(file,"%d",&app->count)!=1) app->count=0; fclose(file); }
    if(saved && savedSize==sizeof(int)) memcpy(&app->count,saved,sizeof(int));
    if(app->count<0 || app->count>999999) app->count=0;
    auto* cb=activity->callbacks;
    cb->onNativeWindowCreated=windowCreated; cb->onNativeWindowDestroyed=windowDestroyed;
    cb->onNativeWindowResized=windowRedraw; cb->onNativeWindowRedrawNeeded=windowRedraw;
    cb->onInputQueueCreated=inputCreated; cb->onInputQueueDestroyed=inputDestroyed;
    cb->onContentRectChanged=contentChanged; cb->onDestroy=destroy;
    cb->onPause=pauseApp; cb->onResume=resumeApp; cb->onSaveInstanceState=saveState;
    __android_log_print(ANDROID_LOG_INFO,"native_buttons","C++ activity created, count=%d",app->count);
}
