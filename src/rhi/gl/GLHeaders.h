#pragma once

#if defined(__APPLE__)
    #ifndef GL_SILENCE_DEPRECATION
        #define GL_SILENCE_DEPRECATION
    #endif
    #include <OpenGL/gl3.h>
#else
    #include <GL/glew.h>
#endif
