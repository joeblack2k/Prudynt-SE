#ifndef VERSION_H_
#define VERSION_H_

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define COMPILE_TIME __DATE__ " " __TIME__
#define BUILD_COMMIT "ef14848-dev1775559171"

#define VERSION COMPILE_TIME "_" "ef14848-dev1775559171"
#define FULL_VERSION_STRING "prudynt-t v" COMPILE_TIME " [" "ef14848-dev1775559171" "]"

#endif   // VERSION_H_
