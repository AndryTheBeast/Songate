// Songate · appicons.inc
// Songate as the home screen (Android, 2.0.1): the Apps button's and the Home screen page's symbols, and Songate's own symbols for
// the kinds of app a device has (the user, 2026-11-13: "create some songate themed icons from android apps"), drawn in the line
// style of icons.inc and plicons.inc. Kinds, never another maker's logo: a browser is a globe, mail an envelope. Include after
// plicons.inc.
#pragma once

#define APP_SVG R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">)SVG"

inline constexpr char ICON_APPS[] = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" fill="#ffffff"><circle cx="5.5" cy="5.5" r="1.7"/><circle cx="12" cy="5.5" r="1.7"/><circle cx="18.5" cy="5.5" r="1.7"/><circle cx="5.5" cy="12" r="1.7"/><circle cx="12" cy="12" r="1.7"/><circle cx="18.5" cy="12" r="1.7"/><circle cx="5.5" cy="18.5" r="1.7"/><circle cx="12" cy="18.5" r="1.7"/><circle cx="18.5" cy="18.5" r="1.7"/></svg>)SVG";
inline constexpr char ICON_HOMESCREEN[] = APP_SVG R"SVG(<path d="M4 10.8 12 4l8 6.8V19a1.5 1.5 0 0 1-1.5 1.5H15v-6H9v6H5.5A1.5 1.5 0 0 1 4 19Z"/></svg>)SVG";

inline constexpr char APP_CAMERA[] = APP_SVG R"SVG(<path d="M4 8.5a2 2 0 0 1 2-2h1.6l1.2-2h6.4l1.2 2H18a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2Z"/><circle cx="12" cy="12.7" r="3.3"/></svg>)SVG";
inline constexpr char APP_CLOCK[] = APP_SVG R"SVG(<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/></svg>)SVG";
inline constexpr char APP_GLOBE[] = APP_SVG R"SVG(<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17"/><path d="M12 3.5c2.6 2.4 3.8 5.2 3.8 8.5s-1.2 6.1-3.8 8.5c-2.6-2.4-3.8-5.2-3.8-8.5s1.2-6.1 3.8-8.5Z"/></svg>)SVG";
inline constexpr char APP_MAIL[] = APP_SVG R"SVG(<rect x="3.5" y="5.5" width="17" height="13" rx="2.2"/><path d="m4.5 7.5 7.5 5.5 7.5-5.5"/></svg>)SVG";
inline constexpr char APP_PHONE[] = APP_SVG R"SVG(<path d="M6.5 3.5h3l1.5 4-2 1.5a11 11 0 0 0 6 6l1.5-2 4 1.5v3a2 2 0 0 1-2.2 2A16.5 16.5 0 0 1 4.5 5.7a2 2 0 0 1 2-2.2Z"/></svg>)SVG";
inline constexpr char APP_CHAT[] = APP_SVG R"SVG(<path d="M5.5 4.5h13a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H11l-4.5 3.5V16.5h-1a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2Z"/></svg>)SVG";
inline constexpr char APP_CALC[] = APP_SVG R"SVG(<rect x="5" y="3.5" width="14" height="17" rx="2.2"/><path d="M8.5 7.5h7"/><path d="M8.5 12h.01M12 12h.01M15.5 12h.01M8.5 16h.01M12 16h.01M15.5 16h.01"/></svg>)SVG";
inline constexpr char APP_BAG[] = APP_SVG R"SVG(<path d="M5.5 8h13l-1 11.5a1.5 1.5 0 0 1-1.5 1.3H8a1.5 1.5 0 0 1-1.5-1.3Z"/><path d="M9 8V7a3 3 0 0 1 6 0v1"/></svg>)SVG";
inline constexpr char APP_PIN[] = APP_SVG R"SVG(<path d="M12 21s-6.5-5.6-6.5-10.5a6.5 6.5 0 0 1 13 0C18.5 15.4 12 21 12 21Z"/><circle cx="12" cy="10.5" r="2.3"/></svg>)SVG";
inline constexpr char APP_VIDEO[] = APP_SVG R"SVG(<rect x="3.5" y="5.5" width="17" height="13" rx="2.5"/><path d="M10.3 9.3v5.4l4.5-2.7Z" fill="#ffffff"/></svg>)SVG";
inline constexpr char APP_PERSON[] = APP_SVG R"SVG(<circle cx="12" cy="8" r="3.6"/><path d="M4.5 20.5c.9-3.9 3.9-6 7.5-6s6.600 2.100 7.500 6"/></svg>)SVG";
inline constexpr char APP_CLOUD[] = APP_SVG R"SVG(<path d="M7.5 18.5a4.500 4.500 0 0 1-.6-8.960 5.500 5.500 0 0 1 10.600 1.200 3.900 3.900 0 0 1-.5 7.760Z"/></svg>)SVG";
