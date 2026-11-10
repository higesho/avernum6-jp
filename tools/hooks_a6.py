"""Hook points of Avernum 6.exe (Steam build) for gen_hooks.py."""

PATTERNS = [
    ('draw_text', 0x47d130, 0x47d2c4),   # up to the font selection and the line height
    ('text_width', 0x47cfe0, 0x47d0e0),  # width of a string (start of its last character)
    ('draw_line', 0x4805d0, 0x480a2a),
    ('tex_draw', 0x488bb0, 0x488c07),    # texture object's draw method: bound-texture cache
    ('tok_count', 0x46b9e0, 0x46ba52),   # script token estimate: byte classes
]

VALUES = [
    ('draw_text', 'draw_text', 'start', 0x47d130),
    ('draw_line', 'draw_line', 'start', 0x4805d0),
    ('text_width', 'text_width', 'start', 0x47cfe0),
    ('main_target', 'draw_text', 'abs', 0x6bf5cc),
    ('ofs_y', 'draw_text', 'abs', 0x773ddc),
    ('ofs_x', 'draw_text', 'abs', 0x773dd8),
    ('bounds', 'draw_text', 'abs', 0x7734e4),
    ('keep_in', 'draw_text', 'call', 0x481610),
    ('cur_font', 'draw_text', 'abs', 0x773de4),
    ('font14', 'draw_text', 'abs', 0x69cfe8),
    ('font15', 'draw_text', 'abs', 0x69ccd8),
    ('font16', 'draw_text', 'abs', 0x69d2f8),
    ('font24', 'draw_text', 'abs', 0x69d608),
    ('font12', 'draw_text', 'abs', 0x69c9c8),
    ('line_h', 'draw_text', 'abs', 0x773cd4),
    ('pen_y', 'draw_line', 'abs', 0x773dec),
    ('pen_x', 'draw_line', 'abs', 0x773de8),
    ('charmap', 'draw_line', 'abs', 0x69d930),
    ('color_div', 'draw_line', 'abs', 0x5bff58),
    ('font_tex', 'draw_line', 'abs', 0x773ff4),
    ('alpha', 'draw_line', 'abs', 0x6ac7b0),
    ('no_clip', 'draw_line', 'abs', 0x773af0),
    ('clip', 'draw_line', 'abs', 0x7734c4),
    ('zoom_a', 'draw_line', 'abs', 0x773aec),
    ('zoom_b', 'draw_line', 'abs', 0x6ac7b4),
    ('bound_tex', 'tex_draw', 'abs', 0x5d209c),
    ('tok_class', 'tok_count', 'at', 0x46ba1a),
    ('tok_digit', 'tok_count', 'at', 0x46ba22),
    ('tok_other', 'tok_count', 'at', 0x46ba29),
    ('tok_letter', 'tok_count', 'at', 0x46ba4a),
]
