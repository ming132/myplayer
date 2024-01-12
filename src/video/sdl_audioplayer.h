#ifndef SDL_AUDIOPLAYER_H
#define SDL_AUDIOPLAYER_H
#include "HVideoPlayer.h"
#include "SDL2/SDL.h"
#include "iostream"
class SDL_audioplayer:public HAudioPlayer
{
public:
    SDL_audioplayer():HAudioPlayer(){
        Init=0;
        if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
               Init=1;
        }
    };
    ~ SDL_audioplayer()
    {
        if(Init)
            SDL_CloseAudioDevice(device_id);
    }
    int Init;
    virtual void update();
    virtual int get_remainder();
    SDL_AudioSpec spec;
    SDL_AudioDeviceID device_id;
};

#endif // SDL_AUDIOPLAYER_H
