#include "orca/model/model_sdk_c.h"
#include <stdio.h>
#include <string.h>
static int transport(const char *method,const char *path,const char *body,void *user,const char **response)
{
    (void)body;
    if(strcmp(method,"GET") || strcmp(path,"/v1/ai/models") || user!=(void*)1)
        return 400;
    *response="{\"models\":[],\"schemaVersion\":1}";
    return 200;
}
int main(void)
{
    orca_model_context* c = orca_model_create("consumer-workspace");
    char* r               = 0;
    int result;
    if (!c || !strstr(orca_model_version(), "1.0.0"))
        return 1;
    result =
        orca_model_execute(c,
                           "{\"schemaVersion\":1,\"operation\":\"create\",\"vertices\":[[0,0,0],[1,0,0],[0,1,0]],\"triangles\":[[0,1,2]]}",
                           0, 0, &r);
    if (result || !r || !strstr(r, "\"ok\":true"))
        return 2;
    orca_model_free_string(r);
    if(orca_model_set_platform_transport(c,transport,(void*)1)!=ORCA_MODEL_OK)return 6;
    result=orca_model_execute(c,"{\"schemaVersion\":1,\"operation\":\"platformAI\",\"command\":\"models\"}",0,0,&r);
    if(result || !r || !strstr(r,"\"httpStatus\":200"))return 7;
    orca_model_free_string(r);
    if(orca_model_set_slicing_backend(c,"missing-library")!=ORCA_MODEL_IO)return 8;
    result = orca_model_execute(c, "{\"schemaVersion\":1,\"operation\":\"transform\",\"expectedRevision\":1,\"translation\":[2,3,4]}", 0, 0,
                                &r);
    if (result || !r || !strstr(r, "\"revision\":2"))
        return 3;
    orca_model_free_string(r);
    result = orca_model_execute(c, "{\"schemaVersion\":1,\"operation\":\"undo\",\"expectedRevision\":2}", 0, 0, &r);
    if (result || !r || !strstr(r, "\"revision\":3"))
        return 4;
    orca_model_free_string(r);
    orca_model_destroy(c);
    orca_model_destroy(c);
    result = orca_model_execute(c, "{}", 0, 0, &r);
    if (!result)
        return 5;
    orca_model_free_string(r);
    puts("C consumer create/transform/undo/platform route/missing backend/invalid handle passed");
    return 0;
}
