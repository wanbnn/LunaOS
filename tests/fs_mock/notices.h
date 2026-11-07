#ifndef LUNA_FS_TEST_NOTICES_H
#define LUNA_FS_TEST_NOTICES_H
static const char font_license[] = "Test fixture notice";
static const char icon_license[] = "Test fixture icon notice";
static const struct {
    const char *name;
    const char *text;
} extra_notices[] = {{"/etc/licenses/fixture.txt","test"}};
#endif
