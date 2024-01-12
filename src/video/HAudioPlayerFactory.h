#ifndef HAUDIOPLAYERFACTORY_H
#define HAUDIOPLAYERFACTORY_H
#include "hmedia.h"
#include "HVideoPlayer.h"
// #include "HVideoCapture.h"
#include "hffplayer.h"
#include "sdl_audioplayer.h"
class HAudioPlayerFactory
{
public:
    static HAudioPlayer* create(audio_player_type type) {
        switch (type) {
        case Audio_SDL:
            return new SDL_audioplayer;
        case Audio_QT:
            // return new HVideoCapture;
            return new SDL_audioplayer;
        default:
            return NULL;
        }
    }
};

#endif // HAUDIOPLAYERFACTORY_H
