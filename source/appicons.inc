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

// More kinds (the user, 2026-11-18: icons for the apps most used on Android; still kinds, drawn in Songate's lines)
inline constexpr char APP_STREAM[] = APP_SVG R"SVG(<circle cx="12" cy="12" r="8.5"/><path d="M7.5 9.5c3-1 6.5-.6 9 .8M8 12.6c2.6-.7 5.3-.4 7.3.7M8.6 15.4c2-.5 4-.3 5.6.5"/></svg>)SVG";
inline constexpr char APP_PLAYCIRCLE[] = APP_SVG R"SVG(<circle cx="12" cy="12" r="8.5"/><path d="M10.5 9v6l4.8-3Z"/></svg>)SVG";
inline constexpr char APP_WAVES[] = APP_SVG R"SVG(<path d="M4 12h1.5M7.5 8v8M11 5v14M14.5 8.5v7M18 10.5v3"/></svg>)SVG";
inline constexpr char APP_VINYL[] = APP_SVG R"SVG(<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="2.2"/><path d="M12 6.5a5.5 5.5 0 0 1 5.5 5.5"/></svg>)SVG";
inline constexpr char APP_CLOUDNOTE[] = APP_SVG R"SVG(<path d="M7 18a4 4 0 0 1-.6-7.95A5.5 5.5 0 0 1 17 8.5a4 4 0 0 1 .5 9.5Z"/><path d="M11 15.5v-4l3-.6"/></svg>)SVG";
inline constexpr char APP_PODCAST[] = APP_SVG R"SVG(<circle cx="12" cy="11" r="2"/><path d="M8.5 15a5 5 0 1 1 7 0M6 17.5a8.5 8.5 0 1 1 12 0M12 13v8"/></svg>)SVG";
inline constexpr char APP_BOOK[] = APP_SVG R"SVG(<path d="M5 5.5A1.5 1.5 0 0 1 6.5 4H19v14H6.5A1.5 1.5 0 0 0 5 19.5ZM5 19.5A1.5 1.5 0 0 0 6.5 21H19"/></svg>)SVG";
inline constexpr char APP_FILM[] = APP_SVG R"SVG(<rect x="3.5" y="4.5" width="17" height="15" rx="2.5"/><path d="M7.5 4.5v15M16.5 4.5v15M3.5 9.5h4M3.5 14.5h4M16.5 9.5h4M16.5 14.5h4"/></svg>)SVG";
inline constexpr char APP_PLANE[] = APP_SVG R"SVG(<path d="M20.5 4 3.5 11l6 2.2L12 19.5l3-5.5ZM9.5 13.2 20.5 4"/></svg>)SVG";
inline constexpr char APP_SNAP[] = APP_SVG R"SVG(<rect x="3.5" y="3.5" width="17" height="17" rx="5"/><circle cx="12" cy="12" r="4"/><path d="M17 7h.01"/></svg>)SVG";
inline constexpr char APP_GAME[] = APP_SVG R"SVG(<path d="M7 8h10a4 4 0 0 1 3.9 4.8l-.8 3.6a2.3 2.3 0 0 1-3.9 1L14.5 15.5h-5l-1.7 1.9a2.3 2.3 0 0 1-3.9-1l-.8-3.6A4 4 0 0 1 7 8Z"/><path d="M8.5 11v3M7 12.5h3M15.5 12h.01M17 13.5h.01"/></svg>)SVG";
inline constexpr char APP_ALIEN[] = APP_SVG R"SVG(<circle cx="12" cy="13" r="6.5"/><path d="M12 6.5 13 3l3.5.8M9.5 12.5h.01M14.5 12.5h.01M9.5 15.5c1.6 1 3.4 1 5 0"/></svg>)SVG";

// Eleven more (the user, 2026-11-19: the sheet's eleven twins removed, these drawn in their place)
inline constexpr char APP_WALLET[] = APP_SVG R"SVG(<path d="M4 7.5A2.5 2.5 0 0 1 6.5 5H18v3"/><rect x="4" y="8" width="16.5" height="11.5" rx="2.4"/><path d="M20.5 11.5h-4a2.2 2.2 0 0 0 0 4.5h4"/><path d="M16.5 13.8h.01"/></svg>)SVG";
inline constexpr char APP_NEWS[] = APP_SVG R"SVG(<rect x="3.5" y="4.5" width="14" height="15" rx="2"/><path d="M17.5 8.5h2a1.5 1.5 0 0 1 1.5 1.5v7.5a2 2 0 0 1-4 0"/><path d="M7 8.5h7M7 12h7M7 15.5h4"/></svg>)SVG";
inline constexpr char APP_KEY[] = APP_SVG R"SVG(<circle cx="8" cy="15.5" r="4"/><path d="m11 12.5 8.5-8.5M16.5 7l2.5 2.5M14 9.5l2 2"/></svg>)SVG";
inline constexpr char APP_SCISSORS[] = APP_SVG R"SVG(<circle cx="6.5" cy="17.5" r="2.8"/><circle cx="6.5" cy="6.5" r="2.8"/><path d="M8.6 8.4 20 17M8.6 15.6 20 7"/></svg>)SVG";
inline constexpr char APP_PALETTE[] = APP_SVG R"SVG(<path d="M12 3.5a8.5 8.5 0 1 0 0 17c1.2 0 1.8-.8 1.8-1.7 0-1.6-1.6-1.8-1.6-3.1 0-.9.8-1.7 1.8-1.7h2.5a4 4 0 0 0 4-4C20.5 6.6 16.7 3.5 12 3.5Z"/><circle cx="7.5" cy="11" r="1" fill="#ffffff" stroke="none"/><circle cx="10.5" cy="7.5" r="1" fill="#ffffff" stroke="none"/><circle cx="15" cy="7.8" r="1" fill="#ffffff" stroke="none"/></svg>)SVG";
inline constexpr char APP_TRANSLATE[] = APP_SVG R"SVG(<path d="M3.5 6h9M8 4v2c0 3.5-1.8 6.5-4.5 8M5.5 9.5c1 2 3 3.6 5.5 4.5"/><path d="m12.5 20 3.8-9 3.8 9M13.8 17h5"/></svg>)SVG";
inline constexpr char APP_TICKET[] = APP_SVG R"SVG(<path d="M3.5 9.5V8a2 2 0 0 1 2-2h13a2 2 0 0 1 2 2v1.5a2.5 2.5 0 0 0 0 5V16a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2v-1.5a2.5 2.5 0 0 0 0-5Z"/><path d="M14.5 7.5v1.8M14.5 11.1v1.8M14.5 14.7v1.8"/></svg>)SVG";
inline constexpr char APP_GIFT[] = APP_SVG R"SVG(<rect x="3.5" y="8.5" width="17" height="4" rx="1.2"/><path d="M5 12.5v6.5a1.5 1.5 0 0 0 1.5 1.5h11a1.5 1.5 0 0 0 1.5-1.5v-6.5M12 8.5v12"/><path d="M12 8.5C10.5 5 7 4.5 7 6.5S10 8.5 12 8.5c2 0 5 0 5-2s-3.5-1.5-5 2Z"/></svg>)SVG";
inline constexpr char APP_TRAIN[] = APP_SVG R"SVG(<rect x="5.5" y="3.5" width="13" height="14" rx="3"/><path d="M5.5 10.5h13M9 14h.01M15 14h.01M8.5 17.5 6.5 20.5M15.5 17.5l2 3"/></svg>)SVG";
inline constexpr char APP_PENCIL[] = APP_SVG R"SVG(<path d="M15.5 4.5a2.1 2.1 0 0 1 3 3L8 18l-4 1 1-4Z"/><path d="m13.5 6.5 3 3"/></svg>)SVG";
inline constexpr char APP_FOOD[] = APP_SVG R"SVG(<path d="M7 3.5v17M4.5 3.5V8a2.5 2.5 0 0 0 5 0V3.5"/><path d="M17 20.5v-17c-2.2.6-3.5 3-3.5 6.5v3H17"/></svg>)SVG";
