#include "sdl_audioplayer.h"
#include "iostream"
int SDL_audioplayer::Init=0;
void SDL_audioplayer::update()
{
    //std::cout<<Init<<" "<<player_cnt<<"\n";
    if(Init==0||device_init==0)
        return ;
    if(last_frame.buf.len==0)
        return ;
    if(player_cnt==0)
    {
        spec.freq=last_frame.sample_rate;
        spec.channels=last_frame.chanels;
        spec.format=(last_frame.type==1)?AUDIO_S16SYS:AUDIO_S32SYS;
        spec.samples=last_frame.buf.len/8;
        spec.silence = 0;
        spec.callback=nullptr;
        device_id=SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, false);
        std::cout<<"device_id: "<<device_id<<" "<<SDL_GetError()<<"\n";
        if (device_id == 0) {
            device_init=0;
            return ;
        }
        SDL_PauseAudioDevice(device_id, 0);
    }
//    remainder=SDL_GetQueuedAudioSize(device_id)/(last_frame.buf.len/8);
//    std::cout<<"cunhuo: "<<SDL_GetQueuedAudioSize(device_id)<<" "<<remainder<<"\n";
    SDL_QueueAudio(device_id,last_frame.buf.base,last_frame.buf.len/8);
    while(SDL_GetQueuedAudioSize(device_id)>=4*last_frame.buf.len/8)
        SDL_Delay(1);
    //std::cout<<"get_remainder: "<<get_remainder()<<"\n";
    player_cnt++;
    return ;
}
int SDL_audioplayer::get_remainder()
{
//    if(Init!=2)
//        return 0;
//    if(last_frame.buf.len==0)
//        return 0;
//    //    std::cout<<last_frame.duration<<" "<<SDL_GetQueuedAudioSize(device_id)/(last_frame.buf.len/8)<<"\n";
//    return SDL_GetQueuedAudioSize(device_id)/(last_frame.buf.len/8);
    return 0;
}
