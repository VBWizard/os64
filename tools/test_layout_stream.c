#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"

int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb");if(!input)return 2;
    fseek(input,0,SEEK_END);long size=ftell(input);rewind(input);
    if(size<0 || size>8*1024*1024)return 2;
    char *html=malloc((size_t)size+1);
    if(!html || fread(html,1,(size_t)size,input)!=(size_t)size)return 2;
    html[size]=0;fclose(input);
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=32*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"stream fixture text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"stream fixture font");
    loop_page(html);free(html);
    if(!g.stream.diag)g.stream.diag=diag_new_claimed("http://fixture.test/stream-article",1);
    s_layout_profile=true;
    check(loop_settle() && !g.stream.active && g.page.tree && !flow_incomplete(g.page.tree),
          "captured article finishes through the real parser/window path");
    const char *report=probe_text_of("layout-stream-report");
    check(report && strstr(report,"\"state\": \"complete\""),"fixed parser checkpoints complete");
    const char *stream=yonder_diag_fact_value(g.page.diag,"layout stream");
    check(stream && strstr(stream,"0 refused/partial") && strstr(stream,"0 clock errors"),
          "stream phase record is complete");
    check(nsheet_jobs==0 && nscript_jobs==0,"fixture does not depend on external sheets/scripts");
    FILE *out=fopen(argv[2],"w");
    if(out){fprintf(out,"%s\nlayout stream: %s\n",report?report:"missing",stream?stream:"missing");fclose(out);}
    else check(false,"fixture report output written");
    s_layout_profile=false;loop_drop();os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"stream fixture text released");
    check(live==0,"stream fixture heap empty");
    printf("Stream measurement fixture: %u checks, %u failed\n",checks,failures);
    return failures?1:0;
}
