/* WinRT Windows.Globalization.Calendar implementation
 *
 * Copyright 2026 Giang Nguyen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <stdlib.h>

#include "private.h"
#include "winnls.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

#include "timezones.h"

/* Only the Gregorian calendar is implemented.  Times are kept as UTC ticks since 1601-01-01
 * (Windows.Foundation.DateTime); fields are derived in the local time of the calendar's time
 * zone, whose rules come from the Windows time zone of the same CLDR mapping. */

#define TICKS_PER_SECOND  ((INT64)10000000)
#define TICKS_PER_MINUTE  (60 * TICKS_PER_SECOND)
#define TICKS_PER_HOUR    (60 * TICKS_PER_MINUTE)
#define TICKS_PER_DAY     (24 * TICKS_PER_HOUR)

/* 0001-01-01T00:00:00 and 9999-12-31T23:59:59.9999999 */
#define MIN_TICKS  ((INT64)-504911232000000000)
#define MAX_TICKS  ((INT64)2650467743999999999)

static const WCHAR gregorian_calendar[] = L"GregorianCalendar";
static const WCHAR clock_12h[] = L"12HourClock";
static const WCHAR clock_24h[] = L"24HourClock";

/* calendar systems Windows accepts, but that are not implemented here */
static const WCHAR *other_calendars[] =
{
    L"HebrewCalendar", L"HijriCalendar", L"JapaneseCalendar", L"JulianCalendar", L"KoreanCalendar",
    L"PersianCalendar", L"TaiwanCalendar", L"ThaiCalendar", L"UmAlQuraCalendar",
};

static const struct numeral_system
{
    const WCHAR *name;
    WCHAR zero;
} numeral_systems[] =
{
    {L"Arab", 0x0660}, {L"ArabExt", 0x06f0}, {L"Bali", 0x1b50}, {L"Beng", 0x09e6}, {L"Cham", 0xaa50},
    {L"Deva", 0x0966}, {L"FullWide", 0xff10}, {L"Gujr", 0x0ae6}, {L"Guru", 0x0a66}, {L"Java", 0xa9d0},
    {L"Kali", 0xa900}, {L"Khmr", 0x17e0}, {L"Knda", 0x0ce6}, {L"Lana", 0x1a80}, {L"LanaTham", 0x1a90},
    {L"Laoo", 0x0ed0}, {L"Latn", '0'}, {L"Lepc", 0x1c40}, {L"Limb", 0x1946}, {L"Mlym", 0x0d66},
    {L"Mong", 0x1810}, {L"Mtei", 0xabf0}, {L"Mymr", 0x1040}, {L"MymrShan", 0x1090}, {L"Nkoo", 0x07c0},
    {L"Olck", 0x1c50}, {L"Orya", 0x0b66}, {L"Saur", 0xa8d0}, {L"Sund", 0x1bb0}, {L"Talu", 0x19d0},
    {L"TamlDec", 0x0be6}, {L"Telu", 0x0c66}, {L"Thai", 0x0e50}, {L"Tibt", 0x0f20}, {L"Vaii", 0xa620},
};

struct calendar
{
    ICalendar ICalendar_iface;
    ITimeZoneOnCalendar ITimeZoneOnCalendar_iface;
    LONG ref;

    INT64 time;
    int nanoseconds; /* below the resolution of time, 0-99 */
    BOOL twelve_hour;
    const struct numeral_system *numerals;
    const struct timezone_entry *zone;
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    BOOL have_rules;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    HSTRING *languages;
    UINT32 language_count;
};

struct fields
{
    int year, month, day, hour, minute, second;
    INT64 fraction;  /* ticks within the second */
    int day_of_week;
};

static const ICalendarVtbl calendar_vtbl;

static inline struct calendar *impl_from_ICalendar( ICalendar *iface )
{
    return CONTAINING_RECORD( iface, struct calendar, ICalendar_iface );
}

static struct calendar *unsafe_impl_from_ICalendar( ICalendar *iface )
{
    if (!iface || iface->lpVtbl != &calendar_vtbl) return NULL;
    return impl_from_ICalendar( iface );
}

/*
 * Date arithmetic on the proleptic Gregorian calendar, days counted from 1601-01-01.
 */

static INT64 floor_div( INT64 a, INT64 b )
{
    INT64 q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}

static BOOL is_leap_year( int year )
{
    return !(year % 4) && ((year % 100) || !(year % 400));
}

static int days_in_month( int year, int month )
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year( year )) return 29;
    return days[month - 1];
}

static INT64 days_from_civil( int year, int month, int day )
{
    INT64 era, y = year - (month <= 2);
    unsigned int yoe, doy, doe;

    era = floor_div( y, 400 );
    yoe = y - era * 400;
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + 134774;
}

static void civil_from_days( INT64 days, int *year, int *month, int *day )
{
    INT64 era, z = days - 134774 + 719468;
    unsigned int doe, yoe, doy, mp;

    era = floor_div( z, 146097 );
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = yoe + era * 400 + (*month <= 2);
}

static int day_of_week( INT64 days )
{
    /* 1601-01-01 was a Monday */
    return (int)((days % 7 + 8) % 7);
}

static void fields_from_local( INT64 local, struct fields *f )
{
    INT64 days = floor_div( local, TICKS_PER_DAY ), rem = local - days * TICKS_PER_DAY;

    civil_from_days( days, &f->year, &f->month, &f->day );
    f->day_of_week = day_of_week( days );
    f->hour = rem / TICKS_PER_HOUR;
    f->minute = (rem % TICKS_PER_HOUR) / TICKS_PER_MINUTE;
    f->second = (rem % TICKS_PER_MINUTE) / TICKS_PER_SECOND;
    f->fraction = rem % TICKS_PER_SECOND;
}

static INT64 local_from_fields( const struct fields *f )
{
    return days_from_civil( f->year, f->month, f->day ) * TICKS_PER_DAY + f->hour * TICKS_PER_HOUR
           + f->minute * TICKS_PER_MINUTE + f->second * TICKS_PER_SECOND + f->fraction;
}

/*
 * Time zones
 */

static const struct timezone_entry *find_timezone( const WCHAR *id )
{
    const WCHAR *canonical = id;
    int min, max, pos, res;

    min = 0;
    max = ARRAY_SIZE(timezone_aliases) - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        if (!(res = wcsicmp( id, timezone_aliases[pos].alias )))
        {
            canonical = timezone_aliases[pos].id;
            break;
        }
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }

    min = 0;
    max = ARRAY_SIZE(timezones) - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        if (!(res = wcsicmp( canonical, timezones[pos].id ))) return &timezones[pos];
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
    return NULL;
}

static const struct timezone_entry *find_timezone_for_key( const WCHAR *key )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(timezones); i++)
        if (timezones[i].primary && !wcsicmp( timezones[i].windows, key )) return &timezones[i];
    return NULL;
}

static void set_timezone( struct calendar *calendar, const struct timezone_entry *zone )
{
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    DWORD i;

    calendar->zone = zone;
    calendar->have_rules = FALSE;
    memset( &calendar->dtzi, 0, sizeof(calendar->dtzi) );

    for (i = 0; !EnumDynamicTimeZoneInformation( i, &dtzi ); i++)
    {
        if (wcsicmp( dtzi.TimeZoneKeyName, zone->windows )) continue;
        calendar->dtzi = dtzi;
        calendar->have_rules = TRUE;
        return;
    }
    WARN( "no rules for time zone %s (%s), using UTC.\n", debugstr_w(zone->id), debugstr_w(zone->windows) );
}

static void set_default_timezone( struct calendar *calendar )
{
    const struct timezone_entry *zone = NULL;
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;

    if (GetDynamicTimeZoneInformation( &dtzi ) != TIME_ZONE_ID_INVALID)
        zone = find_timezone_for_key( dtzi.TimeZoneKeyName );
    if (!zone)
    {
        WARN( "unknown time zone %s, using UTC.\n", debugstr_w(dtzi.TimeZoneKeyName) );
        zone = find_timezone( L"Etc/UTC" );
    }
    set_timezone( calendar, zone );
}

struct zone_rules
{
    INT64 standard;  /* offsets from UTC, in ticks */
    INT64 daylight;
    BOOL has_dst;
    INT64 dst_start;  /* UTC instants of the transitions in the year */
    INT64 dst_end;
};

static INT64 transition_date( int year, const SYSTEMTIME *rule )
{
    INT64 days;
    int day;

    if (rule->wYear) day = rule->wDay;
    else
    {
        day = 1 + (rule->wDayOfWeek - day_of_week( days_from_civil( year, rule->wMonth, 1 ) ) + 7) % 7
              + (rule->wDay - 1) * 7;
        while (day > days_in_month( year, rule->wMonth )) day -= 7;
    }
    days = days_from_civil( year, rule->wMonth, day );
    return days * TICKS_PER_DAY + rule->wHour * TICKS_PER_HOUR + rule->wMinute * TICKS_PER_MINUTE
           + rule->wSecond * TICKS_PER_SECOND + rule->wMilliseconds * (TICKS_PER_SECOND / 1000);
}

static void get_zone_rules( struct calendar *calendar, int year, struct zone_rules *rules )
{
    TIME_ZONE_INFORMATION tzi;

    memset( rules, 0, sizeof(*rules) );
    if (!calendar->have_rules) return;

    /* The Windows rules describe the present; before 1601 only the standard offset is used. */
    if (year < 1601 || !GetTimeZoneInformationForYear( min( year, 30827 ), &calendar->dtzi, &tzi ))
    {
        rules->standard = rules->daylight = -(INT64)(calendar->dtzi.Bias + calendar->dtzi.StandardBias) * TICKS_PER_MINUTE;
        return;
    }

    rules->standard = -(INT64)(tzi.Bias + tzi.StandardBias) * TICKS_PER_MINUTE;
    rules->daylight = -(INT64)(tzi.Bias + tzi.DaylightBias) * TICKS_PER_MINUTE;
    if (!tzi.StandardDate.wMonth || !tzi.DaylightDate.wMonth || rules->standard == rules->daylight)
    {
        rules->daylight = rules->standard;
        return;
    }
    rules->has_dst = TRUE;
    /* daylight time starts at a local standard time, and ends at a local daylight time */
    rules->dst_start = transition_date( year, &tzi.DaylightDate ) - rules->standard;
    rules->dst_end = transition_date( year, &tzi.StandardDate ) - rules->daylight;
}

static INT64 get_offset( struct calendar *calendar, INT64 time, BOOL *dst )
{
    struct zone_rules rules;
    struct fields f;
    BOOL in_dst;

    get_zone_rules( calendar, 0, &rules );
    fields_from_local( time + rules.standard, &f );
    get_zone_rules( calendar, f.year, &rules );

    if (!rules.has_dst) in_dst = FALSE;
    else if (rules.dst_start < rules.dst_end) in_dst = time >= rules.dst_start && time < rules.dst_end;
    else in_dst = time < rules.dst_end || time >= rules.dst_start;

    if (dst) *dst = in_dst;
    return in_dst ? rules.daylight : rules.standard;
}

/* Converts a local time to UTC.  An ambiguous local time keeps the offset of the current time;
 * a local time in a gap fails when strict, and otherwise moves forward by the size of the gap. */
static HRESULT utc_from_local( struct calendar *calendar, INT64 local, BOOL strict, INT64 *time )
{
    INT64 current = get_offset( calendar, calendar->time, NULL ), offsets[2], found[2];
    struct zone_rules rules;
    struct fields f;
    unsigned int i, count = 0;

    fields_from_local( local, &f );
    get_zone_rules( calendar, f.year, &rules );
    offsets[0] = rules.standard;
    offsets[1] = rules.daylight;

    for (i = 0; i < (rules.has_dst ? 2 : 1); i++)
        if (get_offset( calendar, local - offsets[i], NULL ) == offsets[i]) found[count++] = local - offsets[i];

    if (count == 2)
    {
        if (local - found[1] == current) *time = found[1];
        else if (local - found[0] == current) *time = found[0];
        else *time = min( found[0], found[1] );
    }
    else if (count == 1) *time = found[0];
    else if (strict) return E_INVALIDARG;
    else *time = local - min( rules.standard, rules.daylight );

    return S_OK;
}

static void get_fields( struct calendar *calendar, struct fields *f )
{
    fields_from_local( calendar->time + get_offset( calendar, calendar->time, NULL ), f );
}

static HRESULT set_fields( struct calendar *calendar, struct fields *f, BOOL strict )
{
    INT64 time;
    HRESULT hr;

    if (f->year < 1 || f->year > 9999) return E_INVALIDARG;
    if (FAILED(hr = utc_from_local( calendar, local_from_fields( f ), strict, &time ))) return hr;
    calendar->time = time;
    return S_OK;
}

static HRESULT set_time( struct calendar *calendar, INT64 time )
{
    struct fields f;

    if (time < MIN_TICKS || time > MAX_TICKS) return E_INVALIDARG;
    fields_from_local( time + get_offset( calendar, time, NULL ), &f );
    if (f.year < 1 || f.year > 9999) return E_INVALIDARG;
    calendar->time = time;
    return S_OK;
}

static INT64 get_system_time(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime( &ft );
    return ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/*
 * Strings
 */

static HRESULT create_hstring( const WCHAR *str, HSTRING *out )
{
    return WindowsCreateString( str, wcslen( str ), out );
}

static HRESULT format_number( struct calendar *calendar, INT64 value, int min_digits, HSTRING *out )
{
    WCHAR buffer[32], *end = buffer + ARRAY_SIZE(buffer), *p = end;
    int digits = 0;

    do
    {
        *--p = calendar->numerals->zero + (WCHAR)(value % 10);
        value /= 10;
        digits++;
    } while (value && p > buffer);
    while (digits++ < min_digits && p > buffer) *--p = calendar->numerals->zero;

    return WindowsCreateString( p, end - p, out );
}

static HRESULT format_padded( struct calendar *calendar, INT64 value, INT32 min_digits, HSTRING *out )
{
    if (min_digits <= 0) return E_INVALIDARG;
    return format_number( calendar, value, min( min_digits, 30 ), out );
}

/* Picks, for an ideal length, the longest form not longer than it, or the shortest form when
 * none fits.  An ideal length of zero selects the default form. */
static HRESULT format_name( const WCHAR **forms, unsigned int count, unsigned int default_form, INT32 ideal_length,
                            HSTRING *out )
{
    const WCHAR *best = NULL, *shortest = NULL;
    unsigned int i;
    size_t len;

    if (ideal_length < 0) return E_INVALIDARG;
    if (!ideal_length) return create_hstring( forms[default_form], out );

    for (i = 0; i < count; i++)
    {
        len = wcslen( forms[i] );
        if (!shortest || len < wcslen( shortest )) shortest = forms[i];
        if (len <= ideal_length && (!best || len > wcslen( best ))) best = forms[i];
    }
    return create_hstring( best ? best : shortest, out );
}

static void get_calendar_info( struct calendar *calendar, CALTYPE type, WCHAR *buffer, int len )
{
    if (!GetCalendarInfoEx( calendar->locale, CAL_GREGORIAN, NULL, type, buffer, len, NULL )) buffer[0] = 0;
}

static void get_locale_info( struct calendar *calendar, LCTYPE type, WCHAR *buffer, int len )
{
    if (!GetLocaleInfoEx( calendar->locale, type, buffer, len )) buffer[0] = 0;
}

/*
 * Languages
 */

static void free_languages( struct calendar *calendar )
{
    UINT32 i;

    for (i = 0; i < calendar->language_count; i++) WindowsDeleteString( calendar->languages[i] );
    free( calendar->languages );
    calendar->languages = NULL;
    calendar->language_count = 0;
}

static HRESULT copy_languages( struct calendar *dst, const struct calendar *src )
{
    HSTRING *languages;
    UINT32 i;

    if (!(languages = calloc( src->language_count, sizeof(*languages) ))) return E_OUTOFMEMORY;
    for (i = 0; i < src->language_count; i++) WindowsDuplicateString( src->languages[i], &languages[i] );
    free_languages( dst );
    dst->languages = languages;
    dst->language_count = src->language_count;
    return S_OK;
}

/* Resolves the locale from the language list; invalid tags are skipped. */
static void resolve_locale( struct calendar *calendar )
{
    const WCHAR *tag;
    UINT32 i;

    for (i = 0; i < calendar->language_count; i++)
    {
        tag = WindowsGetStringRawBuffer( calendar->languages[i], NULL );
        if (!IsValidLocaleName( tag )) continue;
        lstrcpynW( calendar->locale, tag, ARRAY_SIZE(calendar->locale) );
        return;
    }
    GetUserDefaultLocaleName( calendar->locale, ARRAY_SIZE(calendar->locale) );
}

static HRESULT set_languages( struct calendar *calendar, IIterable_HSTRING *iterable )
{
    IIterator_HSTRING *iterator;
    HSTRING *languages = NULL, *tmp, value;
    UINT32 count = 0, i;
    boolean has_current;
    HRESULT hr;

    if (!iterable)
    {
        WCHAR locale[LOCALE_NAME_MAX_LENGTH];

        if (!GetUserDefaultLocaleName( locale, ARRAY_SIZE(locale) )) return E_FAIL;
        if (!(languages = malloc( sizeof(*languages) ))) return E_OUTOFMEMORY;
        if (FAILED(hr = create_hstring( locale, &languages[0] )))
        {
            free( languages );
            return hr;
        }
        count = 1;
    }
    else
    {
        if (FAILED(hr = IIterable_HSTRING_First( iterable, &iterator ))) return hr;
        for (hr = IIterator_HSTRING_get_HasCurrent( iterator, &has_current ); SUCCEEDED(hr) && has_current;
             hr = IIterator_HSTRING_MoveNext( iterator, &has_current ))
        {
            if (FAILED(hr = IIterator_HSTRING_get_Current( iterator, &value ))) break;
            if (WindowsIsStringEmpty( value ) || !(tmp = realloc( languages, (count + 1) * sizeof(*languages) )))
            {
                hr = WindowsIsStringEmpty( value ) ? E_INVALIDARG : E_OUTOFMEMORY;
                WindowsDeleteString( value );
                break;
            }
            languages = tmp;
            languages[count++] = value;
        }
        IIterator_HSTRING_Release( iterator );
        if (SUCCEEDED(hr) && !count) hr = E_INVALIDARG;
        if (FAILED(hr))
        {
            for (i = 0; i < count; i++) WindowsDeleteString( languages[i] );
            free( languages );
            return hr;
        }
    }

    free_languages( calendar );
    calendar->languages = languages;
    calendar->language_count = count;
    resolve_locale( calendar );
    return S_OK;
}

static const struct numeral_system *find_numeral_system( const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(numeral_systems); i++)
        if (!wcsicmp( name, numeral_systems[i].name )) return &numeral_systems[i];
    return NULL;
}

static void set_locale_defaults( struct calendar *calendar )
{
    WCHAR buffer[80];
    BOOL quoted = FALSE;
    unsigned int i;

    calendar->twelve_hour = FALSE;
    get_locale_info( calendar, LOCALE_STIMEFORMAT, buffer, ARRAY_SIZE(buffer) );
    for (i = 0; buffer[i]; i++)
    {
        if (buffer[i] == '\'') quoted = !quoted;
        else if (!quoted && buffer[i] == 'h') calendar->twelve_hour = TRUE;
    }

    calendar->numerals = find_numeral_system( L"Latn" );
    get_locale_info( calendar, LOCALE_SNATIVEDIGITS, buffer, ARRAY_SIZE(buffer) );
    for (i = 0; i < ARRAY_SIZE(numeral_systems); i++)
        if (buffer[0] == numeral_systems[i].zero) calendar->numerals = &numeral_systems[i];
}

static HRESULT check_calendar_system( HSTRING value )
{
    const WCHAR *name = WindowsGetStringRawBuffer( value, NULL );
    unsigned int i;

    if (!wcscmp( name, gregorian_calendar )) return S_OK;
    for (i = 0; i < ARRAY_SIZE(other_calendars); i++)
    {
        if (wcscmp( name, other_calendars[i] )) continue;
        FIXME( "calendar system %s not implemented.\n", debugstr_w(name) );
        return E_NOTIMPL;
    }
    return E_INVALIDARG;
}

static HRESULT parse_clock( HSTRING value, BOOL *twelve_hour )
{
    const WCHAR *name = WindowsGetStringRawBuffer( value, NULL );

    if (!wcscmp( name, clock_12h )) *twelve_hour = TRUE;
    else if (!wcscmp( name, clock_24h )) *twelve_hour = FALSE;
    else return E_INVALIDARG;
    return S_OK;
}

/*
 * ICalendar
 */

static HRESULT WINAPI calendar_QueryInterface( ICalendar *iface, REFIID iid, void **out )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ICalendar ))
    {
        IInspectable_AddRef( (*out = &impl->ICalendar_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_ITimeZoneOnCalendar ))
    {
        IInspectable_AddRef( (*out = &impl->ITimeZoneOnCalendar_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI calendar_AddRef( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI calendar_Release( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    if (!ref)
    {
        free_languages( impl );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI calendar_GetIids( ICalendar *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids );

    if (!(*iids = CoTaskMemAlloc( 2 * sizeof(IID) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_ICalendar;
    (*iids)[1] = IID_ITimeZoneOnCalendar;
    *iid_count = 2;
    return S_OK;
}

static HRESULT WINAPI calendar_GetRuntimeClassName( ICalendar *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( RuntimeClass_Windows_Globalization_Calendar,
                                ARRAY_SIZE(RuntimeClass_Windows_Globalization_Calendar) - 1, class_name );
}

static HRESULT WINAPI calendar_GetTrustLevel( ICalendar *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT calendar_copy( struct calendar *dst, const struct calendar *src )
{
    HRESULT hr;

    if (dst == src) return S_OK;
    if (FAILED(hr = copy_languages( dst, src ))) return hr;
    dst->time = src->time;
    dst->nanoseconds = src->nanoseconds;
    dst->twelve_hour = src->twelve_hour;
    dst->numerals = src->numerals;
    dst->zone = src->zone;
    dst->dtzi = src->dtzi;
    dst->have_rules = src->have_rules;
    memcpy( dst->locale, src->locale, sizeof(dst->locale) );
    return S_OK;
}

static const ITimeZoneOnCalendarVtbl time_zone_vtbl;

static HRESULT calendar_alloc( struct calendar **out )
{
    struct calendar *calendar;

    if (!(calendar = calloc( 1, sizeof(*calendar) ))) return E_OUTOFMEMORY;
    calendar->ICalendar_iface.lpVtbl = &calendar_vtbl;
    calendar->ITimeZoneOnCalendar_iface.lpVtbl = &time_zone_vtbl;
    calendar->ref = 1;
    *out = calendar;
    return S_OK;
}

static HRESULT WINAPI calendar_Clone( ICalendar *iface, ICalendar **value )
{
    struct calendar *impl = impl_from_ICalendar( iface ), *clone;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!value) return E_POINTER;
    if (FAILED(hr = calendar_alloc( &clone ))) return hr;
    if (FAILED(hr = calendar_copy( clone, impl )))
    {
        ICalendar_Release( &clone->ICalendar_iface );
        return hr;
    }
    *value = &clone->ICalendar_iface;
    return S_OK;
}

static HRESULT WINAPI calendar_SetToMin( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f = {1, 1, 1, 0, 0, 0, 0};
    HRESULT hr;

    TRACE( "iface %p.\n", iface );
    if (SUCCEEDED(hr = set_fields( impl, &f, FALSE ))) impl->nanoseconds = 0;
    return hr;
}

static HRESULT WINAPI calendar_SetToMax( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f = {9999, 12, 31, 23, 59, 59, TICKS_PER_SECOND - 1};
    HRESULT hr;

    TRACE( "iface %p.\n", iface );
    if (SUCCEEDED(hr = set_fields( impl, &f, FALSE ))) impl->nanoseconds = 99;
    return hr;
}

static HRESULT WINAPI calendar_get_Languages( ICalendar *iface, IVectorView_HSTRING **value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    HSTRING *languages;
    UINT32 i;
    HRESULT hr;

    TRACE( "iface %p, value %p.\n", iface, value );

    if (!(languages = calloc( impl->language_count, sizeof(*languages) ))) return E_OUTOFMEMORY;
    for (i = 0; i < impl->language_count; i++) WindowsDuplicateString( impl->languages[i], &languages[i] );
    hr = hstring_vector_create( languages, impl->language_count, value );
    if (FAILED(hr)) for (i = 0; i < impl->language_count; i++) WindowsDeleteString( languages[i] );
    free( languages );
    return hr;
}

static HRESULT WINAPI calendar_get_NumeralSystem( ICalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return create_hstring( impl->numerals->name, value );
}

static HRESULT WINAPI calendar_put_NumeralSystem( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    const struct numeral_system *numerals;

    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );

    if (!(numerals = find_numeral_system( WindowsGetStringRawBuffer( value, NULL ) ))) return E_INVALIDARG;
    impl->numerals = numerals;
    return S_OK;
}

static HRESULT WINAPI calendar_GetCalendarSystem( ICalendar *iface, HSTRING *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return create_hstring( gregorian_calendar, value );
}

static HRESULT WINAPI calendar_ChangeCalendarSystem( ICalendar *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return check_calendar_system( value );
}

static HRESULT WINAPI calendar_GetClock( ICalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return create_hstring( impl->twelve_hour ? clock_12h : clock_24h, value );
}

static HRESULT WINAPI calendar_ChangeClock( ICalendar *iface, HSTRING value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return parse_clock( value, &impl->twelve_hour );
}

static HRESULT WINAPI calendar_GetDateTime( ICalendar *iface, DateTime *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, result %p.\n", iface, result );
    result->UniversalTime = impl->time;
    return S_OK;
}

static HRESULT WINAPI calendar_SetDateTime( ICalendar *iface, DateTime value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    HRESULT hr;

    TRACE( "iface %p, value %I64d.\n", iface, value.UniversalTime );
    if (SUCCEEDED(hr = set_time( impl, value.UniversalTime ))) impl->nanoseconds = 0;
    return hr;
}

static HRESULT WINAPI calendar_SetToNow( ICalendar *iface )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    HRESULT hr;

    TRACE( "iface %p.\n", iface );
    if (SUCCEEDED(hr = set_time( impl, get_system_time() ))) impl->nanoseconds = 0;
    return hr;
}

static HRESULT WINAPI calendar_get_FirstEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfEras( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Era( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Era( ICalendar *iface, INT32 value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    return value == 1 ? S_OK : E_INVALIDARG;
}

static HRESULT WINAPI calendar_AddEras( ICalendar *iface, INT32 eras )
{
    TRACE( "iface %p, eras %d.\n", iface, eras );
    return eras ? E_INVALIDARG : S_OK;
}

static HRESULT era_as_string( struct calendar *calendar, INT32 ideal_length, BOOL full, HSTRING *result )
{
    WCHAR abbrev[80], name[80];
    const WCHAR *forms[] = {abbrev, name};

    get_calendar_info( calendar, CAL_SABBREVERASTRING, abbrev, ARRAY_SIZE(abbrev) );
    get_calendar_info( calendar, CAL_SERASTRING, name, ARRAY_SIZE(name) );
    if (!abbrev[0]) wcscpy( abbrev, name );
    if (full) return create_hstring( name, result );
    return format_name( forms, ARRAY_SIZE(forms), 1, ideal_length, result );
}

static HRESULT WINAPI calendar_EraAsFullString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return era_as_string( impl_from_ICalendar( iface ), 0, TRUE, result );
}

static HRESULT WINAPI calendar_EraAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return era_as_string( impl_from_ICalendar( iface ), ideal_length, FALSE, result );
}

static HRESULT WINAPI calendar_get_FirstYearInThisEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastYearInThisEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 9999;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfYearsInThisEra( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 9999;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Year( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.year;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Year( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > 9999) return E_INVALIDARG;
    get_fields( impl, &f );
    f.year = value;
    f.day = min( f.day, days_in_month( f.year, f.month ) );
    return set_fields( impl, &f, TRUE );
}

static HRESULT add_months( struct calendar *calendar, INT64 months )
{
    struct fields f;
    INT64 total;

    get_fields( calendar, &f );
    total = (INT64)f.year * 12 + f.month - 1 + months;
    if (total < 12 || total >= 10000 * 12) return E_INVALIDARG;
    f.year = total / 12;
    f.month = total % 12 + 1;
    f.day = min( f.day, days_in_month( f.year, f.month ) );
    return set_fields( calendar, &f, FALSE );
}

static HRESULT WINAPI calendar_AddYears( ICalendar *iface, INT32 years )
{
    TRACE( "iface %p, years %d.\n", iface, years );
    return add_months( impl_from_ICalendar( iface ), (INT64)years * 12 );
}

static HRESULT WINAPI calendar_YearAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, f.year, 1, result );
}

static HRESULT WINAPI calendar_YearAsTruncatedString( ICalendar *iface, INT32 remaining_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 value, modulus = 1;
    struct fields f;
    INT32 i;

    TRACE( "iface %p, remaining_digits %d, result %p.\n", iface, remaining_digits, result );

    if (remaining_digits <= 0) return E_INVALIDARG;
    get_fields( impl, &f );
    value = f.year;
    for (i = 0; i < remaining_digits && modulus <= value; i++) modulus *= 10;
    return format_padded( impl, value % modulus, remaining_digits, result );
}

static HRESULT WINAPI calendar_YearAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, f.year, min_digits, result );
}

static HRESULT WINAPI calendar_get_FirstMonthInThisYear( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastMonthInThisYear( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 12;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfMonthsInThisYear( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 12;
    return S_OK;
}

static HRESULT WINAPI calendar_get_Month( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.month;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Month( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > 12) return E_INVALIDARG;
    get_fields( impl, &f );
    f.month = value;
    f.day = min( f.day, days_in_month( f.year, f.month ) );
    return set_fields( impl, &f, TRUE );
}

static HRESULT WINAPI calendar_AddMonths( ICalendar *iface, INT32 months )
{
    TRACE( "iface %p, months %d.\n", iface, months );
    return add_months( impl_from_ICalendar( iface ), months );
}

static HRESULT month_as_string( struct calendar *calendar, INT32 ideal_length, BOOL full, HSTRING *result )
{
    WCHAR abbrev[80], name[80];
    const WCHAR *forms[] = {abbrev, name};
    struct fields f;

    get_fields( calendar, &f );
    get_calendar_info( calendar, CAL_SABBREVMONTHNAME1 + f.month - 1, abbrev, ARRAY_SIZE(abbrev) );
    get_calendar_info( calendar, CAL_SMONTHNAME1 + f.month - 1, name, ARRAY_SIZE(name) );
    if (full) return create_hstring( name, result );
    return format_name( forms, ARRAY_SIZE(forms), 0, ideal_length, result );
}

static HRESULT WINAPI calendar_MonthAsFullString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return month_as_string( impl_from_ICalendar( iface ), 0, TRUE, result );
}

static HRESULT WINAPI calendar_MonthAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return month_as_string( impl_from_ICalendar( iface ), ideal_length, FALSE, result );
}

static HRESULT WINAPI calendar_MonthAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return month_as_string( impl_from_ICalendar( iface ), 0, TRUE, result );
}

static HRESULT WINAPI calendar_MonthAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return month_as_string( impl_from_ICalendar( iface ), ideal_length, FALSE, result );
}

static HRESULT WINAPI calendar_MonthAsNumericString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, f.month, 1, result );
}

static HRESULT WINAPI calendar_MonthAsPaddedNumericString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, f.month, min_digits, result );
}

static HRESULT add_days( struct calendar *calendar, INT64 days )
{
    struct fields f;
    INT64 local;

    get_fields( calendar, &f );
    local = local_from_fields( &f ) + days * TICKS_PER_DAY;
    if (local < MIN_TICKS || local > MAX_TICKS) return E_INVALIDARG;
    fields_from_local( local, &f );
    return set_fields( calendar, &f, FALSE );
}

static HRESULT WINAPI calendar_AddWeeks( ICalendar *iface, INT32 weeks )
{
    TRACE( "iface %p, weeks %d.\n", iface, weeks );
    return add_days( impl_from_ICalendar( iface ), (INT64)weeks * 7 );
}

static HRESULT WINAPI calendar_get_FirstDayInThisMonth( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastDayInThisMonth( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = days_in_month( f.year, f.month );
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfDaysInThisMonth( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return calendar_get_LastDayInThisMonth( iface, value );
}

static HRESULT WINAPI calendar_get_Day( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.day;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Day( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    get_fields( impl, &f );
    if (value < 1 || value > days_in_month( f.year, f.month )) return E_INVALIDARG;
    f.day = value;
    return set_fields( impl, &f, TRUE );
}

static HRESULT WINAPI calendar_AddDays( ICalendar *iface, INT32 days )
{
    TRACE( "iface %p, days %d.\n", iface, days );
    return add_days( impl_from_ICalendar( iface ), days );
}

static HRESULT WINAPI calendar_DayAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, f.day, 1, result );
}

static HRESULT WINAPI calendar_DayAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, f.day, min_digits, result );
}

static HRESULT WINAPI calendar_get_DayOfWeek( ICalendar *iface, DayOfWeek *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.day_of_week;
    return S_OK;
}

static HRESULT day_of_week_as_string( struct calendar *calendar, INT32 ideal_length, BOOL full, HSTRING *result )
{
    WCHAR shortest[80], abbrev[80], name[80];
    const WCHAR *forms[] = {shortest, abbrev, name};
    struct fields f;
    int index;

    get_fields( calendar, &f );
    index = (f.day_of_week + 6) % 7; /* CAL_SDAYNAME1 is Monday */
    get_calendar_info( calendar, CAL_SSHORTESTDAYNAME1 + index, shortest, ARRAY_SIZE(shortest) );
    get_calendar_info( calendar, CAL_SABBREVDAYNAME1 + index, abbrev, ARRAY_SIZE(abbrev) );
    get_calendar_info( calendar, CAL_SDAYNAME1 + index, name, ARRAY_SIZE(name) );
    if (!shortest[0]) wcscpy( shortest, abbrev );
    if (full) return create_hstring( name, result );
    return format_name( forms, ARRAY_SIZE(forms), 1, ideal_length, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsFullString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return day_of_week_as_string( impl_from_ICalendar( iface ), 0, TRUE, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return day_of_week_as_string( impl_from_ICalendar( iface ), ideal_length, FALSE, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsFullSoloString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return day_of_week_as_string( impl_from_ICalendar( iface ), 0, TRUE, result );
}

static HRESULT WINAPI calendar_DayOfWeekAsSoloString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return day_of_week_as_string( impl_from_ICalendar( iface ), ideal_length, FALSE, result );
}

static HRESULT WINAPI calendar_get_FirstPeriodInThisDay( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastPeriodInThisDay( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->twelve_hour ? 2 : 1;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfPeriodsInThisDay( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    return calendar_get_LastPeriodInThisDay( iface, value );
}

static HRESULT WINAPI calendar_get_Period( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl, &f );
    *value = impl->twelve_hour && f.hour >= 12 ? 2 : 1;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Period( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 1 || value > (impl->twelve_hour ? 2 : 1)) return E_INVALIDARG;
    if (!impl->twelve_hour) return S_OK;
    get_fields( impl, &f );
    f.hour = f.hour % 12 + (value == 2 ? 12 : 0);
    return set_fields( impl, &f, TRUE );
}

static HRESULT add_ticks( struct calendar *calendar, INT64 ticks )
{
    if (ticks > MAX_TICKS - MIN_TICKS || ticks < MIN_TICKS - MAX_TICKS) return E_INVALIDARG;
    return set_time( calendar, calendar->time + ticks );
}

static HRESULT WINAPI calendar_AddPeriods( ICalendar *iface, INT32 periods )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, periods %d.\n", iface, periods );

    if (impl->twelve_hour) return add_ticks( impl, (INT64)periods * 12 * TICKS_PER_HOUR );
    return add_days( impl, periods );
}

static HRESULT period_as_string( struct calendar *calendar, INT32 ideal_length, BOOL full, HSTRING *result )
{
    WCHAR narrow[2], name[80];
    const WCHAR *forms[] = {narrow, name};
    struct fields f;

    if (ideal_length < 0) return E_INVALIDARG;
    if (!calendar->twelve_hour) return WindowsCreateString( NULL, 0, result );

    get_fields( calendar, &f );
    get_locale_info( calendar, f.hour < 12 ? LOCALE_S1159 : LOCALE_S2359, name, ARRAY_SIZE(name) );
    narrow[0] = name[0];
    narrow[1] = 0;
    if (full) return create_hstring( name, result );
    return format_name( forms, ARRAY_SIZE(forms), 1, ideal_length, result );
}

static HRESULT WINAPI calendar_PeriodAsFullString( ICalendar *iface, HSTRING *result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return period_as_string( impl_from_ICalendar( iface ), 0, TRUE, result );
}

static HRESULT WINAPI calendar_PeriodAsString( ICalendar *iface, INT32 ideal_length, HSTRING *result )
{
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    return period_as_string( impl_from_ICalendar( iface ), ideal_length, FALSE, result );
}

static HRESULT WINAPI calendar_get_FirstHourInThisPeriod( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->twelve_hour ? 12 : 0;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastHourInThisPeriod( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->twelve_hour ? 11 : 23;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfHoursInThisPeriod( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->twelve_hour ? 12 : 24;
    return S_OK;
}

static int display_hour( struct calendar *calendar, const struct fields *f )
{
    if (!calendar->twelve_hour) return f->hour;
    return f->hour % 12 ? f->hour % 12 : 12;
}

static HRESULT WINAPI calendar_get_Hour( ICalendar *iface, INT32 *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl, &f );
    *value = display_hour( impl, &f );
    return S_OK;
}

static HRESULT WINAPI calendar_put_Hour( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    get_fields( impl, &f );
    if (impl->twelve_hour)
    {
        if (value < 1 || value > 12) return E_INVALIDARG;
        f.hour = value % 12 + (f.hour >= 12 ? 12 : 0);
    }
    else
    {
        if (value < 0 || value > 23) return E_INVALIDARG;
        f.hour = value;
    }
    return set_fields( impl, &f, TRUE );
}

static HRESULT WINAPI calendar_AddHours( ICalendar *iface, INT32 hours )
{
    TRACE( "iface %p, hours %d.\n", iface, hours );
    return add_ticks( impl_from_ICalendar( iface ), (INT64)hours * TICKS_PER_HOUR );
}

static HRESULT WINAPI calendar_HourAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, display_hour( impl, &f ), 1, result );
}

static HRESULT WINAPI calendar_HourAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, display_hour( impl, &f ), min_digits, result );
}

static HRESULT WINAPI calendar_get_Minute( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.minute;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Minute( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 0 || value > 59) return E_INVALIDARG;
    get_fields( impl, &f );
    f.minute = value;
    return set_fields( impl, &f, TRUE );
}

static HRESULT WINAPI calendar_AddMinutes( ICalendar *iface, INT32 minutes )
{
    TRACE( "iface %p, minutes %d.\n", iface, minutes );
    return add_ticks( impl_from_ICalendar( iface ), (INT64)minutes * TICKS_PER_MINUTE );
}

static HRESULT WINAPI calendar_MinuteAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, f.minute, 1, result );
}

static HRESULT WINAPI calendar_MinuteAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, f.minute, min_digits, result );
}

static HRESULT WINAPI calendar_get_Second( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.second;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Second( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 0 || value > 59) return E_INVALIDARG;
    get_fields( impl, &f );
    f.second = value;
    return set_fields( impl, &f, TRUE );
}

static HRESULT WINAPI calendar_AddSeconds( ICalendar *iface, INT32 seconds )
{
    TRACE( "iface %p, seconds %d.\n", iface, seconds );
    return add_ticks( impl_from_ICalendar( iface ), (INT64)seconds * TICKS_PER_SECOND );
}

static HRESULT WINAPI calendar_SecondAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, f.second, 1, result );
}

static HRESULT WINAPI calendar_SecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, f.second, min_digits, result );
}

static HRESULT WINAPI calendar_get_Nanosecond( ICalendar *iface, INT32 *value )
{
    struct fields f;
    TRACE( "iface %p, value %p.\n", iface, value );
    get_fields( impl_from_ICalendar( iface ), &f );
    *value = f.fraction * 100 + impl_from_ICalendar( iface )->nanoseconds;
    return S_OK;
}

static HRESULT WINAPI calendar_put_Nanosecond( ICalendar *iface, INT32 value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;
    HRESULT hr;

    TRACE( "iface %p, value %d.\n", iface, value );

    if (value < 0 || value > 999999999) return E_INVALIDARG;
    get_fields( impl, &f );
    f.fraction = value / 100;
    if (SUCCEEDED(hr = set_fields( impl, &f, TRUE ))) impl->nanoseconds = value % 100;
    return hr;
}

static HRESULT WINAPI calendar_AddNanoseconds( ICalendar *iface, INT32 nanoseconds )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    INT64 total = impl->nanoseconds + (INT64)nanoseconds, ticks = floor_div( total, 100 );
    HRESULT hr;

    TRACE( "iface %p, nanoseconds %d.\n", iface, nanoseconds );
    if (SUCCEEDED(hr = add_ticks( impl, ticks ))) impl->nanoseconds = total - ticks * 100;
    return hr;
}

static HRESULT WINAPI calendar_NanosecondAsString( ICalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, result %p.\n", iface, result );
    get_fields( impl, &f );
    return format_number( impl, f.fraction * 100 + impl->nanoseconds, 1, result );
}

static HRESULT WINAPI calendar_NanosecondAsPaddedString( ICalendar *iface, INT32 min_digits, HSTRING *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    struct fields f;

    TRACE( "iface %p, min_digits %d, result %p.\n", iface, min_digits, result );
    get_fields( impl, &f );
    return format_padded( impl, f.fraction * 100 + impl->nanoseconds, min_digits, result );
}

static INT32 compare_time( INT64 a, INT64 b )
{
    return a < b ? -1 : a > b ? 1 : 0;
}

static HRESULT WINAPI calendar_Compare( ICalendar *iface, ICalendar *other, INT32 *result )
{
    struct calendar *impl = impl_from_ICalendar( iface ), *other_impl;
    DateTime time;
    HRESULT hr;

    TRACE( "iface %p, other %p, result %p.\n", iface, other, result );

    *result = 0;
    if (!other) return E_POINTER;
    if (FAILED(hr = ICalendar_GetDateTime( other, &time ))) return hr;
    if (!(*result = compare_time( impl->time, time.UniversalTime )) && (other_impl = unsafe_impl_from_ICalendar( other )))
        *result = compare_time( impl->nanoseconds, other_impl->nanoseconds );
    return S_OK;
}

static HRESULT WINAPI calendar_CompareDateTime( ICalendar *iface, DateTime other, INT32 *result )
{
    struct calendar *impl = impl_from_ICalendar( iface );

    TRACE( "iface %p, other %I64d, result %p.\n", iface, other.UniversalTime, result );
    *result = compare_time( impl->time, other.UniversalTime );
    return S_OK;
}

static HRESULT WINAPI calendar_CopyTo( ICalendar *iface, ICalendar *other )
{
    struct calendar *impl = impl_from_ICalendar( iface ), *target;

    TRACE( "iface %p, other %p.\n", iface, other );

    if (!other) return E_POINTER;
    if (!(target = unsafe_impl_from_ICalendar( other ))) return E_INVALIDARG;
    return calendar_copy( target, impl );
}

static HRESULT WINAPI calendar_get_FirstMinuteInThisHour( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastMinuteInThisHour( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 59;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfMinutesInThisHour( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 60;
    return S_OK;
}

static HRESULT WINAPI calendar_get_FirstSecondInThisMinute( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI calendar_get_LastSecondInThisMinute( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 59;
    return S_OK;
}

static HRESULT WINAPI calendar_get_NumberOfSecondsInThisMinute( ICalendar *iface, INT32 *value )
{
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = 60;
    return S_OK;
}

static HRESULT WINAPI calendar_get_ResolvedLanguage( ICalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return create_hstring( impl->locale, value );
}

static HRESULT WINAPI calendar_get_IsDaylightSavingTime( ICalendar *iface, boolean *value )
{
    struct calendar *impl = impl_from_ICalendar( iface );
    BOOL dst;

    TRACE( "iface %p, value %p.\n", iface, value );
    get_offset( impl, impl->time, &dst );
    *value = dst;
    return S_OK;
}

static const ICalendarVtbl calendar_vtbl =
{
    calendar_QueryInterface,
    calendar_AddRef,
    calendar_Release,
    /* IInspectable methods */
    calendar_GetIids,
    calendar_GetRuntimeClassName,
    calendar_GetTrustLevel,
    /* ICalendar methods */
    calendar_Clone,
    calendar_SetToMin,
    calendar_SetToMax,
    calendar_get_Languages,
    calendar_get_NumeralSystem,
    calendar_put_NumeralSystem,
    calendar_GetCalendarSystem,
    calendar_ChangeCalendarSystem,
    calendar_GetClock,
    calendar_ChangeClock,
    calendar_GetDateTime,
    calendar_SetDateTime,
    calendar_SetToNow,
    calendar_get_FirstEra,
    calendar_get_LastEra,
    calendar_get_NumberOfEras,
    calendar_get_Era,
    calendar_put_Era,
    calendar_AddEras,
    calendar_EraAsFullString,
    calendar_EraAsString,
    calendar_get_FirstYearInThisEra,
    calendar_get_LastYearInThisEra,
    calendar_get_NumberOfYearsInThisEra,
    calendar_get_Year,
    calendar_put_Year,
    calendar_AddYears,
    calendar_YearAsString,
    calendar_YearAsTruncatedString,
    calendar_YearAsPaddedString,
    calendar_get_FirstMonthInThisYear,
    calendar_get_LastMonthInThisYear,
    calendar_get_NumberOfMonthsInThisYear,
    calendar_get_Month,
    calendar_put_Month,
    calendar_AddMonths,
    calendar_MonthAsFullString,
    calendar_MonthAsString,
    calendar_MonthAsFullSoloString,
    calendar_MonthAsSoloString,
    calendar_MonthAsNumericString,
    calendar_MonthAsPaddedNumericString,
    calendar_AddWeeks,
    calendar_get_FirstDayInThisMonth,
    calendar_get_LastDayInThisMonth,
    calendar_get_NumberOfDaysInThisMonth,
    calendar_get_Day,
    calendar_put_Day,
    calendar_AddDays,
    calendar_DayAsString,
    calendar_DayAsPaddedString,
    calendar_get_DayOfWeek,
    calendar_DayOfWeekAsFullString,
    calendar_DayOfWeekAsString,
    calendar_DayOfWeekAsFullSoloString,
    calendar_DayOfWeekAsSoloString,
    calendar_get_FirstPeriodInThisDay,
    calendar_get_LastPeriodInThisDay,
    calendar_get_NumberOfPeriodsInThisDay,
    calendar_get_Period,
    calendar_put_Period,
    calendar_AddPeriods,
    calendar_PeriodAsFullString,
    calendar_PeriodAsString,
    calendar_get_FirstHourInThisPeriod,
    calendar_get_LastHourInThisPeriod,
    calendar_get_NumberOfHoursInThisPeriod,
    calendar_get_Hour,
    calendar_put_Hour,
    calendar_AddHours,
    calendar_HourAsString,
    calendar_HourAsPaddedString,
    calendar_get_Minute,
    calendar_put_Minute,
    calendar_AddMinutes,
    calendar_MinuteAsString,
    calendar_MinuteAsPaddedString,
    calendar_get_Second,
    calendar_put_Second,
    calendar_AddSeconds,
    calendar_SecondAsString,
    calendar_SecondAsPaddedString,
    calendar_get_Nanosecond,
    calendar_put_Nanosecond,
    calendar_AddNanoseconds,
    calendar_NanosecondAsString,
    calendar_NanosecondAsPaddedString,
    calendar_Compare,
    calendar_CompareDateTime,
    calendar_CopyTo,
    calendar_get_FirstMinuteInThisHour,
    calendar_get_LastMinuteInThisHour,
    calendar_get_NumberOfMinutesInThisHour,
    calendar_get_FirstSecondInThisMinute,
    calendar_get_LastSecondInThisMinute,
    calendar_get_NumberOfSecondsInThisMinute,
    calendar_get_ResolvedLanguage,
    calendar_get_IsDaylightSavingTime,
};

/*
 * ITimeZoneOnCalendar
 */

DEFINE_IINSPECTABLE( time_zone, ITimeZoneOnCalendar, struct calendar, ICalendar_iface )

static HRESULT WINAPI time_zone_GetTimeZone( ITimeZoneOnCalendar *iface, HSTRING *value )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return create_hstring( impl->zone->id, value );
}

static HRESULT WINAPI time_zone_ChangeTimeZone( ITimeZoneOnCalendar *iface, HSTRING time_zone_id )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    const struct timezone_entry *zone;

    TRACE( "iface %p, time_zone_id %s.\n", iface, debugstr_hstring( time_zone_id ) );

    if (!(zone = find_timezone( WindowsGetStringRawBuffer( time_zone_id, NULL ) ))) return E_INVALIDARG;
    set_timezone( impl, zone );
    return S_OK;
}

static HRESULT time_zone_as_string( struct calendar *calendar, BOOL full, HSTRING *result )
{
    WCHAR buffer[64];
    INT64 offset;
    BOOL dst;
    int minutes;

    if (!wcscmp( calendar->zone->id, L"Etc/UTC" ))
        return create_hstring( full ? L"Coordinated Universal Time" : L"UTC", result );
    if (!wcscmp( calendar->zone->id, L"Etc/GMT" ))
        return create_hstring( full ? L"Greenwich Mean Time" : L"GMT", result );

    offset = get_offset( calendar, calendar->time, &dst );
    if (full && calendar->have_rules)
        return create_hstring( dst ? calendar->dtzi.DaylightName : calendar->dtzi.StandardName, result );

    minutes = offset / TICKS_PER_MINUTE;
    if (!minutes) return create_hstring( L"GMT", result );
    if (abs( minutes ) % 60)
        swprintf( buffer, ARRAY_SIZE(buffer), L"GMT%c%d:%02d", minutes < 0 ? '-' : '+', abs( minutes ) / 60,
                  abs( minutes ) % 60 );
    else swprintf( buffer, ARRAY_SIZE(buffer), L"GMT%c%d", minutes < 0 ? '-' : '+', abs( minutes ) / 60 );
    return create_hstring( buffer, result );
}

static HRESULT WINAPI time_zone_TimeZoneAsFullString( ITimeZoneOnCalendar *iface, HSTRING *result )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    TRACE( "iface %p, result %p.\n", iface, result );
    return time_zone_as_string( impl, TRUE, result );
}

static HRESULT WINAPI time_zone_TimeZoneAsString( ITimeZoneOnCalendar *iface, INT32 ideal_length, HSTRING *result )
{
    struct calendar *impl = impl_from_ITimeZoneOnCalendar( iface );
    TRACE( "iface %p, ideal_length %d, result %p.\n", iface, ideal_length, result );
    if (ideal_length < 0) return E_INVALIDARG;
    return time_zone_as_string( impl, FALSE, result );
}

static const ITimeZoneOnCalendarVtbl time_zone_vtbl =
{
    time_zone_QueryInterface,
    time_zone_AddRef,
    time_zone_Release,
    /* IInspectable methods */
    time_zone_GetIids,
    time_zone_GetRuntimeClassName,
    time_zone_GetTrustLevel,
    /* ITimeZoneOnCalendar methods */
    time_zone_GetTimeZone,
    time_zone_ChangeTimeZone,
    time_zone_TimeZoneAsFullString,
    time_zone_TimeZoneAsString,
};

/* A NULL argument pointer means "not given"; a given NULL HSTRING is the empty string. */
static HRESULT calendar_create( IIterable_HSTRING *languages, const HSTRING *system, const HSTRING *clock,
                                const HSTRING *time_zone, ICalendar **out )
{
    const struct timezone_entry *zone = NULL;
    struct calendar *calendar;
    BOOL twelve_hour = FALSE;
    HRESULT hr;

    if (system && FAILED(hr = check_calendar_system( *system ))) return hr;
    if (clock && FAILED(hr = parse_clock( *clock, &twelve_hour ))) return hr;
    if (time_zone && !(zone = find_timezone( WindowsGetStringRawBuffer( *time_zone, NULL ) ))) return E_INVALIDARG;

    if (FAILED(hr = calendar_alloc( &calendar ))) return hr;
    if (FAILED(hr = set_languages( calendar, languages )))
    {
        ICalendar_Release( &calendar->ICalendar_iface );
        return hr;
    }
    set_locale_defaults( calendar );
    if (clock) calendar->twelve_hour = twelve_hour;
    if (zone) set_timezone( calendar, zone );
    else set_default_timezone( calendar );
    calendar->time = get_system_time();

    *out = &calendar->ICalendar_iface;
    return S_OK;
}

/*
 * Activation factory
 */

struct calendar_factory
{
    IActivationFactory IActivationFactory_iface;
    ICalendarFactory ICalendarFactory_iface;
    ICalendarFactory2 ICalendarFactory2_iface;
    LONG ref;
};

static inline struct calendar_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct calendar_factory, IActivationFactory_iface );
}

static HRESULT WINAPI activation_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct calendar_factory *factory = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef( (*out = &factory->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_ICalendarFactory ))
    {
        IActivationFactory_AddRef( (*out = &factory->ICalendarFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_ICalendarFactory2 ))
    {
        IActivationFactory_AddRef( (*out = &factory->ICalendarFactory2_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI activation_factory_AddRef( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI activation_factory_Release( IActivationFactory *iface )
{
    struct calendar_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI activation_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI activation_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return E_ILLEGAL_METHOD_CALL;
}

static HRESULT WINAPI activation_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activation_factory_ActivateInstance( IActivationFactory *iface, IInspectable **out )
{
    TRACE( "iface %p, out %p.\n", iface, out );
    return calendar_create( NULL, NULL, NULL, NULL, (ICalendar **)out );
}

static const struct IActivationFactoryVtbl activation_factory_vtbl =
{
    activation_factory_QueryInterface,
    activation_factory_AddRef,
    activation_factory_Release,
    /* IInspectable methods */
    activation_factory_GetIids,
    activation_factory_GetRuntimeClassName,
    activation_factory_GetTrustLevel,
    /* IActivationFactory methods */
    activation_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( calendar_factory, ICalendarFactory, struct calendar_factory, IActivationFactory_iface )

static HRESULT WINAPI calendar_factory_CreateCalendarDefaultCalendarAndClock( ICalendarFactory *iface,
        IIterable_HSTRING *languages, ICalendar **result )
{
    TRACE( "iface %p, languages %p, result %p.\n", iface, languages, result );
    if (!languages) return E_POINTER;
    return calendar_create( languages, NULL, NULL, NULL, result );
}

static HRESULT WINAPI calendar_factory_CreateCalendar( ICalendarFactory *iface, IIterable_HSTRING *languages,
        HSTRING calendar, HSTRING clock, ICalendar **result )
{
    TRACE( "iface %p, languages %p, calendar %s, clock %s, result %p.\n", iface, languages,
           debugstr_hstring( calendar ), debugstr_hstring( clock ), result );
    if (!languages) return E_POINTER;
    return calendar_create( languages, &calendar, &clock, NULL, result );
}

static const struct ICalendarFactoryVtbl calendar_factory_vtbl =
{
    calendar_factory_QueryInterface,
    calendar_factory_AddRef,
    calendar_factory_Release,
    /* IInspectable methods */
    calendar_factory_GetIids,
    calendar_factory_GetRuntimeClassName,
    calendar_factory_GetTrustLevel,
    /* ICalendarFactory methods */
    calendar_factory_CreateCalendarDefaultCalendarAndClock,
    calendar_factory_CreateCalendar,
};

DEFINE_IINSPECTABLE( calendar_factory2, ICalendarFactory2, struct calendar_factory, IActivationFactory_iface )

static HRESULT WINAPI calendar_factory2_CreateCalendarWithTimeZone( ICalendarFactory2 *iface,
        IIterable_HSTRING *languages, HSTRING calendar, HSTRING clock, HSTRING time_zone_id, ICalendar **result )
{
    TRACE( "iface %p, languages %p, calendar %s, clock %s, time_zone_id %s, result %p.\n", iface, languages,
           debugstr_hstring( calendar ), debugstr_hstring( clock ), debugstr_hstring( time_zone_id ), result );
    if (!languages) return E_POINTER;
    return calendar_create( languages, &calendar, &clock, &time_zone_id, result );
}

static const struct ICalendarFactory2Vtbl calendar_factory2_vtbl =
{
    calendar_factory2_QueryInterface,
    calendar_factory2_AddRef,
    calendar_factory2_Release,
    /* IInspectable methods */
    calendar_factory2_GetIids,
    calendar_factory2_GetRuntimeClassName,
    calendar_factory2_GetTrustLevel,
    /* ICalendarFactory2 methods */
    calendar_factory2_CreateCalendarWithTimeZone,
};

static struct calendar_factory factory =
{
    {&activation_factory_vtbl},
    {&calendar_factory_vtbl},
    {&calendar_factory2_vtbl},
    1,
};

IActivationFactory *calendar_factory = &factory.IActivationFactory_iface;
