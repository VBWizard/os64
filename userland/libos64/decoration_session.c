#include "os64/decoration_prepare.h"
#include "os64/appearance.h"
#include "os64/io.h"
#include "os64/signal.h"
#include "os64/str.h"

int os64_decor_generation(uint64_t *generation)
{
    if (!generation) return -1;
    int64_t fd=os64_open("/sys/decorations","r");
    if (fd<0) return -1;
    char text[OS64_APPEARANCE_HEADER_MAX];
    size_t length=0;
    while (length<sizeof(text)) {
        int64_t n=os64_read(fd,text+length,sizeof(text)-length);
        if (n<0) {os64_close(fd);return -1;}
        if (!n) break;
        length+=(size_t)n;
    }
    os64_close(fd);
    size_t body;
    return os64_appearance_header_read(text,length,generation,&body)?0:-1;
}

int os64_decor_current(os64_decor_status_t *status)
{
    if(!status)return -1;
    int64_t fd=os64_open("/sys/decorations","r");
    if(fd<0)return -1;
    char text[OS64_DECOR_STATUS_MAX];size_t used=0;int result=-1;
    while(used<sizeof(text)){
        int64_t n=os64_read(fd,text+used,sizeof(text)-used);
        if(n==OS64_INTERRUPTED)continue;
        if(n<0 || (uint64_t)n>sizeof(text)-used)break;
        if(!n){result=os64_decor_status_read(text,used,status)?0:-1;break;}
        used+=(size_t)n;
    }
    os64_close(fd);return result;
}

int os64_decor_apply(const void *bytes, size_t length, uint64_t expected)
{
    os64_decor_view_t view;
    if (!os64_decor_validate(bytes,length,&view)) return -1;
    int64_t fd=os64_open("/sys/decorations","w");
    if (fd<0) return -1;
    struct {os64_decor_command_t header;uint8_t data[OS64_DECOR_DATA_MAX];} packet;
    packet.header=(os64_decor_command_t){.command=OS64_DECOR_BEGIN,
        .total_bytes=(uint32_t)length,.expected_generation=expected};
    int result=-1;
    if (os64_write(fd,&packet.header,sizeof(packet.header))!=(int64_t)sizeof(packet.header)) goto done;
    for (size_t offset=0;offset<length;) {
        size_t n=length-offset;
        if (n>sizeof(packet.data)) n=sizeof(packet.data);
        packet.header.command=OS64_DECOR_DATA;
        packet.header.offset=(uint32_t)offset;packet.header.data_bytes=(uint32_t)n;
        os64_memcpy(packet.data,(const uint8_t *)bytes+offset,n);
        if (os64_write(fd,&packet,sizeof(packet.header)+n)!=(int64_t)(sizeof(packet.header)+n)) goto done;
        offset+=n;
    }
    packet.header.command=OS64_DECOR_COMMIT;
    packet.header.offset=(uint32_t)length;packet.header.data_bytes=0;
    if (os64_write(fd,&packet.header,sizeof(packet.header))==(int64_t)sizeof(packet.header)) result=0;
done:
    os64_close(fd);
    return result;
}
