/*
 * Copyright © 2010 Chris Double <chris.double@double.co.nz>
 *
 * This program is made available under the ISC license.  See the
 * accompanying file LICENSE for details.
 */
#include <iostream>
#include <fstream>
#include <cassert>
#include <thread>

#define HAVE_STDINT_H 1
extern "C" {
    #include "vpx_decoder.h"
    #include "vp8dx.h"
    #include "nestegg/nestegg.h"
}

#include <SDL/SDL.h>

using namespace std;


// читаем в буффер данные из потока
int ifstream_read(void* buffer, size_t size, void* context) {
    ifstream* f = (ifstream*)context;
    f->read((char*)buffer, size);
    // success = 1
    // eof = 0
    // error = -1
    return f->gcount() == size ? 1 : f->eof() ? 0 : -1;
}

// смещаемся по потоку
int ifstream_seek(int64_t n, int whence, void* context) {
    ifstream* f = (ifstream*)context;
    f->clear();
    ios_base::seekdir dir;

    switch (whence) {
        case NESTEGG_SEEK_SET:{
            dir = fstream::beg;
        }break;
        
        case NESTEGG_SEEK_CUR:{
            dir = fstream::cur;
        }break;
        
        case NESTEGG_SEEK_END:{
            dir = fstream::end;
        }break;
    }
    f->seekg(n, dir);
    if (!f->good()){
        return -1;
    }
    return 0;
}

int64_t ifstream_tell(void* context) {
    ifstream* f = (ifstream*)context;
    return f->tellg();
}


// Запуск воспроизведения видео
void play_webm(char const* name) {
    nestegg* nesteg = NULL;

    // поток чтения из файла
    ifstream infile(name);

    nestegg_io ne_io;
    ne_io.read = ifstream_read;
    ne_io.seek = ifstream_seek;
    ne_io.tell = ifstream_tell;
    ne_io.userdata = (void*)&infile;

    // открываем медиа-контейнер
    int r = nestegg_init(&nesteg, ne_io, NULL /* logger */, -1);
    assert(r == 0);

    // читаем длительность видео
    uint64_t duration = 0;
    r = nestegg_duration(nesteg, &duration);
    assert(r == 0);
    cout << "Duration: " << duration << endl;

    // читаем длительность треков
    unsigned int ntracks = 0;
    r = nestegg_track_count(nesteg, &ntracks);
    assert(r == 0);
    cout << "Tracks: " << ntracks << endl;

    // видео-параметры
    nestegg_video_params vparams;
    vparams.width = 0;
    vparams.height = 0;

    // частота FPS
    int fpsValue = 0;

    vpx_codec_iface_t* interface = NULL;
    for (int i=0; i < ntracks; ++i) {
        // получаем id кодека
        int id = nestegg_track_codec_id(nesteg, i);
        assert(id >= 0);

        // тип трека
        int type = nestegg_track_type(nesteg, i);
        cout << "Track " << i << " codec id: " << id << " type: " << type << " ";

        // Определяем какой кодек будем использовать (указатель на функцию)
        interface = (id == NESTEGG_CODEC_VP9) ? 
                        &vpx_codec_vp9_dx_algo : 
                        &vpx_codec_vp8_dx_algo;

        // если у нас видео поток
        if (type == NESTEGG_TRACK_VIDEO) {
            // получим параметры текущего потока
            r = nestegg_track_video_params(nesteg, i, &vparams);
            assert(r == 0);

            // получим частоту кадров видео
            uint64_t frameDurationNanoSec = 0;
            nestegg_track_default_duration(nesteg, i, &frameDurationNanoSec);
            fpsValue = static_cast<int>(frameDurationNanoSec / 1000.0f / 1000.0f);
            if (fpsValue == 0){
                fpsValue = 24;
            }

            // выводим информацию
            cout << "FPS: " << fpsValue << " Size: " << vparams.width << "x" << vparams.height << " (d: " << vparams.display_width << "x" << vparams.display_height << ")";
        }

        // аудио поток
        if (type == NESTEGG_TRACK_AUDIO) {
            nestegg_audio_params params;
            r = nestegg_track_audio_params(nesteg, i, &params);
            assert(r == 0);
            cout << params.rate << " " << params.channels << " channels " << " depth " << params.depth;
        }
        cout << endl;
    }

    vpx_codec_ctx_t codec;
    int flags = 0; 
    int frame_cnt = 0;
    vpx_codec_err_t res;

    cout << "Using codec: " << vpx_codec_iface_name(interface) << endl;
    cout << endl;

    // Инициализация кодека
    if((res = vpx_codec_dec_init(&codec, interface, NULL, flags))) {
        cerr << "Failed to initialize decoder" << endl;
        return;
    }

    // инит SDL
    r = SDL_Init(SDL_INIT_VIDEO);
    assert(r == 0);

    // режим отображения СДЛ такой же, как размер видео
    SDL_Surface* surface = SDL_SetVideoMode(vparams.display_width,
                                            vparams.display_height,
                                            32,
                                            SDL_SWSURFACE);
    assert(surface);

    // OVERLAY
    SDL_Overlay* overlay = SDL_CreateYUVOverlay(vparams.width,
                                                vparams.height,
                                                SDL_YV12_OVERLAY,
                                                surface);
    assert(overlay);

    int32_t videoLastDrawTime = 0;


    int video_count = 0;
    int audio_count = 0;
    nestegg_packet* packet = 0;

    while (1) {
        // читаем пакет пока не прочитается
        // 1 = keep calling
        // 0 = eof
        // -1 = error
        r = nestegg_read_packet(nesteg, &packet);
        if ((r == 1) && (packet == 0)){
            continue;
        }
        if (r == 0) {
            // переход к началу
            for (int i = 0; i < ntracks; ++i){
                nestegg_track_seek(nesteg, i, 0);
            }
            continue;
        }
        if (r <= 0){
            // выход при завершении
            break;
        }


        // получаем трек из пакета
        unsigned int track = 0;
        r = nestegg_packet_track(packet, &track);
        assert(r == 0);

        // TODO: workaround bug
        // если трек-видео + ограничение по частоте 24fps
        bool isVideo = (nestegg_track_type(nesteg, track) == NESTEGG_TRACK_VIDEO);
        if (isVideo) {
            ++video_count;

            //cout << "video frame: " << video_count << "\t ";

            // количество пакетов
            unsigned int count = 0;
            r = nestegg_packet_count(packet, &count);
            assert(r == 0);

            //cout << "Count: " << count << "\t ";

            int nframes = 0;
            // for (int j=0; j < count; ++j) {
            // проверка проигрывания одного кадра
            for (int j=0; j < 1; ++j) {
                // чтение
                unsigned char* data = NULL;    // нету смысла тратить такты на обнуление?? (= NULL)
                size_t length = 0;             // сколько данных получено
                r = nestegg_packet_data(packet, j, &data, &length);
                assert(r == 0);

                // инфа потока
                vpx_codec_stream_info_t si;
                memset(&si, 0, sizeof(si));
                si.sz = sizeof(si);

                // чтение инфы
                vpx_codec_peek_stream_info(interface, data, length, &si);
                //cout << "keyframe: " << (si.is_kf ? "yes" : "no") << "\t " << "length: " << length << "\t ";

                // Выполнение декодирования кадра
                vpx_codec_err_t e = vpx_codec_decode(&codec, data, length, NULL, 0);
                if (e) {
                    cerr << "Failed to decode frame. error: " << e << endl;
                    return;
                }

                vpx_codec_iter_t iter = NULL;
                vpx_image_t* img = NULL;

                // непосредственное декодирование
                while((img = vpx_codec_get_frame(&codec, &iter))) {
                    unsigned int plane = 0;
                    unsigned int y = 0;

                    // вывод размеров
                    //cout << "h: " << img->d_h << " w: " << img->d_w << endl;

                    // номер кадра
                    nframes++;

                    SDL_Rect rect;
                    rect.x = 0;
                    rect.y = 0;
                    rect.w = vparams.display_width;
                    rect.h = vparams.display_height;

                    // блокировка записи-чтения оверлея
                    SDL_LockYUVOverlay(overlay);
                    // Y
                    for (int y = 0; y < img->d_h; ++y){
                        memcpy( overlay->pixels[0] + (overlay->pitches[0]*y),   // куда
                                img->planes[0] + (img->stride[0]*y),            // откуда
                                overlay->pitches[0]);                           // сколько байт
                    }
                    // U
                    for (int y = 0; y < (img->d_h >> 1); ++y){
                        memcpy( overlay->pixels[1] + (overlay->pitches[1]*y),   // куда
                                img->planes[2] + (img->stride[2]*y),            // откуда
                                overlay->pitches[1]);                           // сколько байт
                    }
                    // V
                    for (int y=0; y < img->d_h>>1; ++y){
                        memcpy( overlay->pixels[2] + (overlay->pitches[2]*y),   // куда
                                img->planes[1] + (img->stride[1]*y),            // откуда
                                overlay->pitches[2]);                           // сколько байт
                    }
                    SDL_UnlockYUVOverlay(overlay);

                    // отображем
                    SDL_DisplayYUVOverlay(overlay, &rect);
                }

                //cout << "nframes: " << nframes;
            }

            //cout << endl;
        }

        // аудио не выводим
        /*if (nestegg_track_type(nesteg, track) == NESTEGG_TRACK_AUDIO) {
            //cout << "audio frame: " << ++audio_count << endl;
        }*/

        // чистим
        nestegg_free_packet(packet);
        packet = 0;

        SDL_Event event;
        if (SDL_PollEvent(&event) == 1) {
            if ((event.type == SDL_KEYDOWN) && event.key.keysym.sym == SDLK_ESCAPE){
                break;
            }
            if ((event.type == SDL_KEYDOWN) && (event.key.keysym.sym == SDLK_SPACE)){
                SDL_WM_ToggleFullScreen(surface);
            }
        }

        // ограничение в 60 кадров в сек
        int32_t sdlNow = SDL_GetTicks();
        float delta = static_cast<float>(sdlNow - videoLastDrawTime)/1000.0f;
        videoLastDrawTime = sdlNow;
        if (delta < 1.0f/fpsValue){
            float delayForSleep = std::max(1.0f/fpsValue - delta, 0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delayForSleep * 1000)));
        }
    }

    if(vpx_codec_destroy(&codec)) {
        cerr << "Failed to destroy codec" << endl;
        return;
    }

    nestegg_destroy(nesteg);
    infile.close();
    if (surface) {
        SDL_FreeSurface(surface);
    }

    SDL_Quit();
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: webm filename" << endl;
        return 1;
    }

    play_webm(argv[1]);


    return 0;
}
