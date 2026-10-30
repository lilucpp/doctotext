#include <cstdlib>

struct SjLj_Function_Context {
    struct SjLj_Function_Context *prev;
    int call_site;
    void *data[4];
    void *personality;
    void *lsda;
    void *jbuf[];
};

#if defined(__GNUC__)
static __thread struct SjLj_Function_Context *__sjlj_fc = nullptr;

extern "C" {

void _Unwind_SjLj_Register(struct SjLj_Function_Context *fc) {
    if (fc) {
        fc->prev = __sjlj_fc;
        __sjlj_fc = fc;
    }
}

void _Unwind_SjLj_Unregister(struct SjLj_Function_Context *fc) {
    if (fc) {
        __sjlj_fc = fc->prev;
    }
}

void _Unwind_SjLj_Resume(void *exc) {
    (void)exc;
    std::abort();
}

int __gxx_personality_sj0(int version, int actions, unsigned long long exc_class, void *ue_header, void *context) {
    (void)version;
    (void)actions;
    (void)exc_class;
    (void)ue_header;
    (void)context;
    return 8; // _URC_CONTINUE_UNWIND
}

}
#endif
