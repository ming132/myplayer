#include "sdl_audioplayer.h"
#include "iostream"
void SDL_audioplayer::update()
{
    if(Init==0)
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
        device_id=SDL_OpenAudioDevice(nullptr, false, &spec, nullptr, false);
        //std::cout<<device_id<<"\n";
        if (device_id == 0) {
            Init=0;
            return ;
        }
        Init=2;
        SDL_PauseAudioDevice(device_id, 0);
    }
    //std::cout<<"cunhuo: "<<SDL_GetQueuedAudioSize(device_id)<<"\n";
    //remainder=SDL_GetQueuedAudioSize(device_id)/(last_frame.buf.len/8);
    SDL_QueueAudio(device_id,last_frame.buf.base,last_frame.buf.len/8);
    while(SDL_GetQueuedAudioSize(device_id)>=4*last_frame.buf.len/8)
        SDL_Delay(1);
    //std::cout<<"get_remainder: "<<get_remainder()<<"\n";
    player_cnt++;
    return ;
}
int SDL_audioplayer::get_remainder()
{
    if(Init!=2)
        return 0;
    if(last_frame.buf.len==0)
        return 0;
    //    std::cout<<last_frame.duration<<" "<<SDL_GetQueuedAudioSize(device_id)/(last_frame.buf.len/8)<<"\n";
    return SDL_GetQueuedAudioSize(device_id)/(last_frame.buf.len/8);
}
