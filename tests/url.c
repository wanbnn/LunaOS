#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
bool net_url_resolve(const char *,const char *,char *,size_t);
static void check(const char *base,const char *ref,const char *want) {
    char out[512];assert(net_url_resolve(base,ref,out,sizeof(out)));assert(!strcmp(out,want));
}
int main(void) {
    check("https://site.test:8443/doc","/style.css","https://site.test:8443/style.css");
    check("https://site.test/doc","//cdn.test/a.css","https://cdn.test/a.css");
    check("http://site.test:8080/a/b","next","http://site.test:8080/a/next");
    check("https://site.test","next","https://site.test/next");
    check("https://site.test/a?old#frag","?new","https://site.test/a?new");
    check("https://site.test/a?old#frag","#new","https://site.test/a?old#new");
    check("https://site.test/a","http://other.test/","http://other.test/");
    check("file:///www/index.html","/style.css","file:///style.css");
    char out[8];assert(!net_url_resolve("https://site.test/","long",out,sizeof(out)) && !*out);
    assert(!net_url_resolve("https://site.test/","javascript:bad",out,sizeof(out)));
    puts("PASS: shared URL joining preserves ports, CDN schemes, relative paths, queries and bounds");
}
