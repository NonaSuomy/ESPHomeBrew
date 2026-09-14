/* The FreeType modules in the NetSurf PAPP (FT_CONFIG_MODULES_H in
 * apps/psram_netsurf/papp.json): TrueType fonts, the autofitter and the
 * anti-aliasing renderer. The "freetype" group there compiles these. */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
