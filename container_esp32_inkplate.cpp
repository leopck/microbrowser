#include "container_esp32_inkplate.h"
#include "Inkplate.h"

container_esp32_inkplate::container_esp32_inkplate(Inkplate *d1)
{
  d1 -> begin();                  //Init Inkplate library (you should call this function ONLY ONCE)
  d1 -> clearDisplay();           //Clear frame buffer of display
  d1 -> display();                //Put clear image on display
  d1 -> setTextSize(2);           //Set text scaling to two (text will be two times bigger)
  d1 -> setCursor(0, 0);          //Set print position
  d1 -> setTextColor(BLACK, WHITE);                 //Set text color to black and background color to white
  // d1 -> println("Starting microbrowser, please hold one");    //Write text
  d1 -> println("Scanning for WiFi networks...");    //Write text
  d1 -> display();   
}

// void container_esp32_ip::set_display(Inkplate d1){

//   //Constructor
// //   this.displayContainer = d1;
//   // d1 = new Inkplate(INKPLATE_1BIT);
//   d1.begin();                  //Init Inkplate library (you should call this function ONLY ONCE)
//   d1.clearDisplay();           //Clear frame buffer of display
//   d1.display();                //Put clear image on display
//   d1.setTextSize(2);           //Set text scaling to two (text will be two times bigger)
//   d1.setCursor(0, 0);          //Set print position
//   d1.setTextColor(BLACK, WHITE);                 //Set text color to black and background color to white
//   d1.println("Starting microbrowser, please hold one");    //Write text
//   d1.display();    
// }

container_esp32_inkplate::~container_esp32_inkplate(void)
{
  
}

// litehtml::uint_ptr create_font(const litehtml::tchar_t* faceName, int size, int weight, litehtml::font_style italic, unsigned int decoration, litehtml::font_metrics* fm)
// {
  
// }
//    virtual litehtml::uint_ptr create_font(const litehtml::tchar_t* faceName, int size, int weight, litehtml::font_style italic, unsigned int decoration, litehtml::font_metrics* fm) override;
//    virtual void delete_font(litehtml::uint_ptr hFont) override;
//    virtual int text_width(const litehtml::tchar_t* text, litehtml::uint_ptr hFont) override;
//    virtual void draw_text(litehtml::uint_ptr hdc, const litehtml::tchar_t* text, litehtml::uint_ptr hFont, litehtml::web_color color, const litehtml::position& pos) override;
//    virtual int pt_to_px(int pt) override;
//    virtual int get_default_font_size() const override;
//    virtual const litehtml::tchar_t*  get_default_font_name() const override;
//    virtual void load_image(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, bool redraw_on_ready) override;
//    virtual void get_image_size(const litehtml::tchar_t* src, const litehtml::tchar_t* baseurl, litehtml::size& sz) override;
//    virtual void draw_background(litehtml::uint_ptr hdc, const litehtml::background_paint& bg) override;
//    virtual void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders, const litehtml::position& draw_pos, bool root) override;
//    virtual void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;
//
//    virtual void set_caption(const litehtml::tchar_t* caption) override;
//    virtual void set_base_url(const litehtml::tchar_t* base_url) override;
//    virtual void link(const std::shared_ptr<litehtml::document> &ptr, const litehtml::element::ptr& el) override;
//    virtual void on_anchor_click(const litehtml::tchar_t* url, const litehtml::element::ptr& el) override;
//    virtual void set_cursor(const litehtml::tchar_t* cursor) override;
//    virtual void transform_text(litehtml::tstring& text, litehtml::text_transform tt) override;
//    virtual void import_css(litehtml::tstring& text, const litehtml::tstring& url, litehtml::tstring& baseurl) override;
//    virtual void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius, bool valid_x, bool valid_y) override;
//    virtual void del_clip() override;
//    
//    virtual void get_client_rect(litehtml::position& client) const override;
//    virtual std::shared_ptr<litehtml::element> create_element(const litehtml::tchar_t *tag_name, const litehtml::string_map &attributes, const std::shared_ptr<litehtml::document> &doc) override;
//    virtual void get_media_features(litehtml::media_features& media) const override;