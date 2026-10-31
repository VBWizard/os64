// The real JS console writes through the fixture's OS output adapter. Keep
// its bytes so the host check can compare the export to the displayed JSON.
static void layout_export_save(const char *report_path)
{
    char path[4096];
    int n=snprintf(path,sizeof(path),"%s.console.log",report_path);
    FILE *file=n>0 && (size_t)n<sizeof(path)?fopen(path,"wb"):NULL;
    check(file && fwrite(output,1,output_len,file)==output_len,"measurement console export saved");
    if(file)fclose(file);
}
