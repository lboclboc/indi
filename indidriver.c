#if 0
INDI Driver Functions

Copyright (C) 2020 by Pawel Soja <kernel32.pl@gmail.com>
Copyright (C) 2003 - 2020 Jasem Mutlaq
Copyright (C) 2003 - 2006 Elwood C. Downey

This library is free software;
you can redistribute it and / or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation;
either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY;
without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library;
if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301  USA

#endif

#include "indidriver.h"

#include "base64.h"
#include "eventloop.h"
#include "indicom.h"
#include "indidevapi.h"
#include "locale_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>

pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAXRBUF 2048

/*! INDI property type */
enum
{
INDI_NUMBER,
INDI_SWITCH,
INDI_TEXT,
INDI_LIGHT,
INDI_BLOB,
INDI_UNKNOWN
};

/* Return index of property property if already cached, -1 otherwise */
int isPropDefined(const char *property_name, const char *device_name)
{
    int i = 0;

    for (i = 0; i < nPropCache; i++)
        if (!strcmp(property_name, propCache[i].propName) && !strcmp(device_name, propCache[i].devName))
            return i;

    return -1;
}

/* output a string expanding special characters into xml/html escape sequences */
static void escapeXML_fputs(const char *src, FILE *stream)
{
    const char *ptr = src;
    const char *replacement;

    for(; *ptr; ++ptr)
    {
        switch(*ptr)
        {
        case  '&': replacement = "&amp;";  break;
        case '\'': replacement = "&apos;"; break;
        case  '"': replacement = "&quot;"; break;
        case  '<': replacement = "&lt;";   break;
        case  '>': replacement = "&gt;";   break;
        default:   replacement = NULL;
        }

        if (replacement != NULL)
        {
            fwrite(src, 1, (size_t)(ptr - src), stream);
            src = ptr + 1;
            fputs(replacement, stream);
        }
    }
    fwrite(src, 1, (size_t)(ptr - src), stream);
}

/* output a string expanding special characters into xml/html escape sequences */
static size_t escapeXML2(char *dst, const char *src, size_t size)
{
    char *ptr = dst; // copy pointer

    // size > 1 - place for null-terminator
    for(; size > 1 && *src; ++src)
    {
        switch (*src)
        {
        case '&':
            if (size < 6) goto finish;
            strncpy(ptr, "&amp;", 6);
            ptr += 5;
            size -= 5;
            break;
        case '\'':
            if (size < 7) goto finish;
            strncpy(ptr, "&apos;", 7);
            ptr += 6;
            size -= 6;
            break;
        case '"':
            if (size < 7) goto finish;
            strncpy(ptr, "&quot;", 7);
            ptr += 6;
            size -= 6;
            break;
        case '<':
            if (size < 5) goto finish;
            strncpy(ptr, "&lt;", 5);
            ptr += 4;
            size -= 4;
            break;
        case '>':
            if (size < 5) goto finish;
            strncpy(ptr, "&gt;", 5);
            ptr += 4;
            size -= 4;
            break;
        default:
            *ptr++ = *src;
            size -= 1;
            break;
        }
    }

finish:
    if (size > 0)
        *ptr = 0;
    return (size_t)(ptr - dst);
}

// unused
#if 0
/* output a string expanding special characters into xml/html escape sequences */
/* N.B. You must free the returned buffer after use! */
static char *escapeXML(const char *src, size_t size)
{
    char *dst = malloc(sizeof(char) * size);
    assert_mem(dst);
    escapeXML2(dst, src, size);
    return dst;
}
#endif

/* tell Client to delete the property with given name on given device, or
 * entire device if !name
 */
void IDDeleteVA(const char *dev, const char *name, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    printf("<delProperty\n  device='%s'\n", dev);
    if (name)
        printf(" name='%s'\n", name);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        // #PS: why not escapeXML_fputs?
        printf("  message='");
        printf("%s'\n", entityXML(message));
    }
    printf("/>\n");
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}

void IDDelete(const char *dev, const char *name, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDeleteVA(dev, name, fmt, ap);
    va_end(ap);
}

/* tell indiserver we want to snoop on the given device/property.
 * name ignored if NULL or empty.
 */
void IDSnoopDevice(const char *snooped_device, const char *snooped_property)
{
    pthread_mutex_lock(&stdout_mutex);
    xmlv1();
    if (snooped_property && snooped_property[0])
        printf("<getProperties version='%g' device='%s' name='%s'/>\n", INDIV, snooped_device, snooped_property);
    else
        printf("<getProperties version='%g' device='%s'/>\n", INDIV, snooped_device);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);
}

/* tell indiserver whether we want BLOBs from the given snooped device.
 * silently ignored if given device is not already registered for snooping.
 */
void IDSnoopBLOBs(const char *snooped_device, const char *snooped_property, BLOBHandling bh)
{
    const char *how;

    switch (bh)
    {
        case B_NEVER:
            how = "Never";
            break;
        case B_ALSO:
            how = "Also";
            break;
        case B_ONLY:
            how = "Only";
            break;
        default:
            return;
    }

    pthread_mutex_lock(&stdout_mutex);
    xmlv1();
    if (snooped_property && snooped_property[0])
        printf("<enableBLOB device='%s' name='%s'>%s</enableBLOB>\n", snooped_device, snooped_property, how);
    else
        printf("<enableBLOB device='%s'>%s</enableBLOB>\n", snooped_device, how);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);
}

/* "INDI" wrappers to the more generic eventloop facility. */

int IEAddCallback(int readfiledes, IE_CBF *fp, void *p)
{
    return (addCallback(readfiledes, (CBF *)fp, p));
}

void IERmCallback(int callbackid)
{
    rmCallback(callbackid);
}

int IEAddTimer(int millisecs, IE_TCF *fp, void *p)
{
    return (addTimer(millisecs, (TCF *)fp, p));
}

void IERmTimer(int timerid)
{
    rmTimer(timerid);
}

int IEAddWorkProc(IE_WPF *fp, void *p)
{
    return (addWorkProc((WPF *)fp, p));
}

void IERmWorkProc(int workprocid)
{
    rmWorkProc(workprocid);
}

int IEDeferLoop(int maxms, int *flagp)
{
    return (deferLoop(maxms, flagp));
}

int IEDeferLoop0(int maxms, int *flagp)
{
    return (deferLoop0(maxms, flagp));
}

/* Update property switches in accord with states and names. */
int IUUpdateSwitch(ISwitchVectorProperty *svp, ISState *states, char *names[], int n)
{
    int i = 0;
    ISwitch *sp;
    char sn[MAXINDINAME];

    assert(svp != NULL && "IUUpdateSwitch SVP is NULL");

    /* store On switch name */
    if (svp->r == ISR_1OFMANY)
    {
        sp = IUFindOnSwitch(svp);
        if (sp)
            strncpy(sn, sp->name, MAXINDINAME);

        IUResetSwitch(svp);
    }

    for (i = 0; i < n; i++)
    {
        sp = IUFindSwitch(svp, names[i]);

        if (!sp)
        {
            svp->s = IPS_IDLE;
            IDSetSwitch(svp, "Error: %s is not a member of %s (%s) property.", names[i], svp->label, svp->name);
            return -1;
        }

        sp->s = states[i];
    }

    /* Consistency checks for ISR_1OFMANY after update. */
    if (svp->r == ISR_1OFMANY)
    {
        int t_count = 0;
        for (i = 0; i < svp->nsp; i++)
        {
            if (svp->sp[i].s == ISS_ON)
                t_count++;
        }
        if (t_count != 1)
        {
            IUResetSwitch(svp);
            sp = IUFindSwitch(svp, sn);
            if (sp)
                sp->s = ISS_ON;
            svp->s = IPS_IDLE;
            IDSetSwitch(svp, "Error: invalid state switch for property %s (%s). %s.", svp->label, svp->name,
                        t_count == 0 ? "No switch is on" : "Too many switches are on");
            return -1;
        }
    }

    return 0;
}

/* Update property numbers in accord with values and names */
int IUUpdateNumber(INumberVectorProperty *nvp, double values[], char *names[], int n)
{
    int i = 0;
    INumber *np;

    assert(nvp != NULL && "IUUpdateNumber NVP is NULL");

    for (i = 0; i < n; i++)
    {
        np = IUFindNumber(nvp, names[i]);
        if (!np)
        {
            nvp->s = IPS_IDLE;
            IDSetNumber(nvp, "Error: %s is not a member of %s (%s) property.", names[i], nvp->label, nvp->name);
            return -1;
        }

        if (values[i] < np->min || values[i] > np->max)
        {
            nvp->s = IPS_ALERT;
            IDSetNumber(nvp, "Error: Invalid range for %s (%s). Valid range is from %g to %g. Requested value is %g",
                        np->label, np->name, np->min, np->max, values[i]);
            return -1;
        }
    }

    /* First loop checks for error, second loop set all values atomically*/
    for (i = 0; i < n; i++)
    {
        np        = IUFindNumber(nvp, names[i]);
        np->value = values[i];
    }

    return 0;
}

/* Update property text in accord with texts and names */
int IUUpdateText(ITextVectorProperty *tvp, char *texts[], char *names[], int n)
{
    int i = 0;
    IText *tp;

    assert(tvp != NULL && "IUUpdateText TVP is NULL");

    for (i = 0; i < n; i++)
    {
        tp = IUFindText(tvp, names[i]);
        if (!tp)
        {
            tvp->s = IPS_IDLE;
            IDSetText(tvp, "Error: %s is not a member of %s (%s) property.", names[i], tvp->label, tvp->name);
            return -1;
        }
    }

    /* First loop checks for error, second loop set all values atomically*/
    for (i = 0; i < n; i++)
    {
        tp = IUFindText(tvp, names[i]);
        IUSaveText(tp, texts[i]);
    }

    return 0;
}

/* Update property BLOB in accord with BLOBs and names */
int IUUpdateBLOB(IBLOBVectorProperty *bvp, int sizes[], int blobsizes[], char *blobs[], char *formats[], char *names[],
                 int n)
{
    int i = 0;
    IBLOB *bp;

    assert(bvp != NULL && "IUUpdateBLOB BVP is NULL");

    for (i = 0; i < n; i++)
    {
        bp = IUFindBLOB(bvp, names[i]);
        if (!bp)
        {
            bvp->s = IPS_IDLE;
            IDSetBLOB(bvp, "Error: %s is not a member of %s (%s) property.", names[i], bvp->label, bvp->name);
            return -1;
        }
    }

    /* First loop checks for error, second loop set all values atomically*/
    for (i = 0; i < n; i++)
    {
        bp = IUFindBLOB(bvp, names[i]);
        IUSaveBLOB(bp, sizes[i], blobsizes[i], blobs[i], formats[i]);
    }

    return 0;
}

int IUSaveBLOB(IBLOB *bp, int size, int blobsize, char *blob, char *format)
{
    bp->bloblen = blobsize;
    bp->size    = size;
    bp->blob    = blob;
    strncpy(bp->format, format, MAXINDIFORMAT);
    return 0;
}

void IUFillSwitch(ISwitch *sp, const char *name, const char *label, ISState s)
{
    escapeXML2(sp->name, name, sizeof(sp->name));

    if (label[0])
        escapeXML2(sp->label, label, sizeof(sp->label));
    else
        strncpy(sp->label, sp->name, sizeof(sp->label));

    sp->s   = s;
    sp->svp = NULL;
    sp->aux = NULL;
}

void IUFillLight(ILight *lp, const char *name, const char *label, IPState s)
{
    escapeXML2(lp->name, name, sizeof(lp->name));

    if (label[0])
        escapeXML2(lp->label, label, sizeof(lp->label));
    else
        strncpy(lp->label, lp->name, sizeof(lp->label));

    lp->s   = s;
    lp->lvp = NULL;
    lp->aux = NULL;
}

void IUFillNumber(INumber *np, const char *name, const char *label, const char *format, double min, double max,
                  double step, double value)
{
    escapeXML2(np->name, name, sizeof(np->name));

    if (label[0])
        escapeXML2(np->label, label, sizeof(np->label));
    else
        strncpy(np->label, np->name, sizeof(np->label));

    strncpy(np->format, format, sizeof(np->format)); // escape unnecessary?

    np->min   = min;
    np->max   = max;
    np->step  = step;
    np->value = value;
    np->nvp   = NULL;
    np->aux0  = NULL;
    np->aux1  = NULL;
}

void IUFillText(IText *tp, const char *name, const char *label, const char *initialText)
{
    escapeXML2(tp->name, name, sizeof(tp->name));

    if (label[0])
        escapeXML2(tp->label, label, sizeof(tp->label));
    else
        strncpy(tp->label, tp->name, sizeof(tp->label));

    if (tp->text && tp->text[0])
        free(tp->text);
    tp->text = NULL;

    tp->tvp  = NULL;
    tp->aux0 = NULL;
    tp->aux1 = NULL;

    if (initialText && initialText[0])
        IUSaveText(tp, initialText);
}

void IUFillBLOB(IBLOB *bp, const char *name, const char *label, const char *format)
{
    memset(bp, 0, sizeof(IBLOB));

    escapeXML2(bp->name, name, sizeof(bp->name));

    if (label[0])
        escapeXML2(bp->label, label, sizeof(bp->label));
    else
        strncpy(bp->label, bp->name, sizeof(bp->label));

    strncpy(bp->format, format, sizeof(bp->format)); // escape unnecessary?

    bp->blob    = 0;
    bp->bloblen = 0;
    bp->size    = 0;
    bp->bvp     = 0;
    bp->aux0    = 0;
    bp->aux1    = 0;
    bp->aux2    = 0;
}

void IUFillSwitchVector(ISwitchVectorProperty *svp, ISwitch *sp, int nsp, const char *dev, const char *name,
                        const char *label, const char *group, IPerm p, ISRule r, double timeout, IPState s)
{
    strncpy(svp->device, dev, sizeof(svp->device)); // escape unnecessary?

    escapeXML2(svp->name, name, sizeof(svp->name));

    if (label[0])
        escapeXML2(svp->label, label, sizeof(svp->label));
    else
        strncpy(svp->label, svp->name, sizeof(svp->label));

    strncpy(svp->group, group, sizeof(svp->group)); // escape unnecessary?
    svp->timestamp[0] = '\0';

    svp->p       = p;
    svp->r       = r;
    svp->timeout = timeout;
    svp->s       = s;
    svp->sp      = sp;
    svp->nsp     = nsp;
}

void IUFillLightVector(ILightVectorProperty *lvp, ILight *lp, int nlp, const char *dev, const char *name,
                       const char *label, const char *group, IPState s)
{
    strncpy(lvp->device, dev, sizeof(lvp->device)); // escape unnecessary?

    escapeXML2(lvp->name, name, sizeof(lvp->name));

    if (label[0])
        escapeXML2(lvp->label, label, sizeof(lvp->label));
    else
        strncpy(lvp->label, lvp->name, sizeof(lvp->label));

    strncpy(lvp->group, group, sizeof(lvp->group)); // escape unnecessary?
    lvp->timestamp[0] = '\0';

    lvp->s   = s;
    lvp->lp  = lp;
    lvp->nlp = nlp;
}

void IUFillNumberVector(INumberVectorProperty *nvp, INumber *np, int nnp, const char *dev, const char *name,
                        const char *label, const char *group, IPerm p, double timeout, IPState s)
{
    strncpy(nvp->device, dev, sizeof(nvp->device)); // escape unnecessary?

    escapeXML2(nvp->name, name, sizeof(nvp->name));

    if (label[0])
        escapeXML2(nvp->label, label, sizeof(nvp->label));
    else
        strncpy(nvp->label, nvp->name, sizeof(nvp->label));

    strncpy(nvp->group, group, sizeof(nvp->group)); // escape unnecessary?
    nvp->timestamp[0] = '\0';

    nvp->p       = p;
    nvp->timeout = timeout;
    nvp->s       = s;
    nvp->np      = np;
    nvp->nnp     = nnp;
}

void IUFillTextVector(ITextVectorProperty *tvp, IText *tp, int ntp, const char *dev, const char *name,
                      const char *label, const char *group, IPerm p, double timeout, IPState s)
{
    strncpy(tvp->device, dev, sizeof(tvp->device)); // escape unnecessary?

    escapeXML2(tvp->name, name, sizeof(tvp->name));

    if (label[0])
        escapeXML2(tvp->label, label, sizeof(tvp->label));
    else
        strncpy(tvp->label, tvp->name, sizeof(tvp->label));

    strncpy(tvp->group, group, sizeof(tvp->group)); // escape unnecessary?
    tvp->timestamp[0] = '\0';

    tvp->p       = p;
    tvp->timeout = timeout;
    tvp->s       = s;
    tvp->tp      = tp;
    tvp->ntp     = ntp;
}

void IUFillBLOBVector(IBLOBVectorProperty *bvp, IBLOB *bp, int nbp, const char *dev, const char *name,
                      const char *label, const char *group, IPerm p, double timeout, IPState s)
{
    memset(bvp, 0, sizeof(IBLOBVectorProperty));
    strncpy(bvp->device, dev, sizeof(bvp->device)); // escape unnecessary?

    escapeXML2(bvp->name, name, sizeof(bvp->name));

    if (label[0])
        escapeXML2(bvp->label, label, sizeof(bvp->label));
    else
        strncpy(bvp->label, bvp->name, sizeof(bvp->label));

    strncpy(bvp->group, group, sizeof(bvp->group)); // escape unnecessary?
    bvp->timestamp[0] = '\0';

    bvp->p       = p;
    bvp->timeout = timeout;
    bvp->s       = s;
    bvp->bp      = bp;
    bvp->nbp     = nbp;
}

/*****************************************************************************
 * convenience functions for use in your implementation of ISSnoopDevice().
 */

/* crack the snooped driver setNumberVector or defNumberVector message into
 * the given INumberVectorProperty.
 * return 0 if type, device and name match and all members are present, else
 * return -1
 */
int IUSnoopNumber(XMLEle *root, INumberVectorProperty *nvp)
{
    char *dev, *name;
    XMLEle *ep;
    int i;

    /* check and crack type, device, name and state */
    if (strcmp(tagXMLEle(root) + 3, "NumberVector") || crackDN(root, &dev, &name, NULL) < 0)
        return (-1);
    if (strcmp(dev, nvp->device) || strcmp(name, nvp->name))
        return (-1); /* not this property */
    (void)crackIPState(findXMLAttValu(root, "state"), &nvp->s);

    /* match each INumber with a oneNumber */
    locale_char_t *orig = indi_locale_C_numeric_push();
    for (i = 0; i < nvp->nnp; i++)
    {
        for (ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (!strcmp(tagXMLEle(ep) + 3, "Number") && !strcmp(nvp->np[i].name, findXMLAttValu(ep, "name")))
            {
                if (f_scansexa(pcdataXMLEle(ep), &nvp->np[i].value) < 0)
                {
                    indi_locale_C_numeric_pop(orig);
                    return (-1); /* bad number format */
                }
                break;
            }
        }
        if (!ep)
        {
            indi_locale_C_numeric_pop(orig);
            return (-1); /* element not found */
        }
    }
    indi_locale_C_numeric_pop(orig);

    /* ok */
    return (0);
}

/* crack the snooped driver setTextVector or defTextVector message into
 * the given ITextVectorProperty.
 * return 0 if type, device and name match and all members are present, else
 * return -1
 */
int IUSnoopText(XMLEle *root, ITextVectorProperty *tvp)
{
    char *dev, *name;
    XMLEle *ep;
    int i;

    /* check and crack type, device, name and state */
    if (strcmp(tagXMLEle(root) + 3, "TextVector") || crackDN(root, &dev, &name, NULL) < 0)
        return (-1);
    if (strcmp(dev, tvp->device) || strcmp(name, tvp->name))
        return (-1); /* not this property */
    (void)crackIPState(findXMLAttValu(root, "state"), &tvp->s);

    /* match each IText with a oneText */
    for (i = 0; i < tvp->ntp; i++)
    {
        for (ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (!strcmp(tagXMLEle(ep) + 3, "Text") && !strcmp(tvp->tp[i].name, findXMLAttValu(ep, "name")))
            {
                IUSaveText(&tvp->tp[i], pcdataXMLEle(ep));
                break;
            }
        }
        if (!ep)
            return (-1); /* element not found */
    }

    /* ok */
    return (0);
}

/* crack the snooped driver setLightVector or defLightVector message into
 * the given ILightVectorProperty. it is not necessary that all ILight names
 * be found.
 * return 0 if type, device and name match, else return -1.
 */
int IUSnoopLight(XMLEle *root, ILightVectorProperty *lvp)
{
    char *dev, *name;
    XMLEle *ep;
    int i;

    /* check and crack type, device, name and state */
    if (strcmp(tagXMLEle(root) + 3, "LightVector") || crackDN(root, &dev, &name, NULL) < 0)
        return (-1);
    if (strcmp(dev, lvp->device) || strcmp(name, lvp->name))
        return (-1); /* not this property */

    (void)crackIPState(findXMLAttValu(root, "state"), &lvp->s);

    /* match each oneLight with one ILight */
    for (ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
    {
        if (!strcmp(tagXMLEle(ep) + 3, "Light"))
        {
            const char *name = findXMLAttValu(ep, "name");
            for (i = 0; i < lvp->nlp; i++)
            {
                if (!strcmp(lvp->lp[i].name, name))
                {
                    if (crackIPState(pcdataXMLEle(ep), &lvp->lp[i].s) < 0)
                    {
                        return (-1); /* unrecognized state */
                    }
                    break;
                }
            }
        }
    }

    /* ok */
    return (0);
}

/* crack the snooped driver setSwitchVector or defSwitchVector message into the
 * given ISwitchVectorProperty. it is not necessary that all ISwitch names be
 * found.
 * return 0 if type, device and name match, else return -1.
 */
int IUSnoopSwitch(XMLEle *root, ISwitchVectorProperty *svp)
{
    char *dev, *name;
    XMLEle *ep;
    int i;

    /* check and crack type, device, name and state */
    if (strcmp(tagXMLEle(root) + 3, "SwitchVector") || crackDN(root, &dev, &name, NULL) < 0)
        return (-1);
    if (strcmp(dev, svp->device) || strcmp(name, svp->name))
        return (-1); /* not this property */
    (void)crackIPState(findXMLAttValu(root, "state"), &svp->s);

    /* match each oneSwitch with one ISwitch */
    for (ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
    {
        if (!strcmp(tagXMLEle(ep) + 3, "Switch"))
        {
            const char *name = findXMLAttValu(ep, "name");
            for (i = 0; i < svp->nsp; i++)
            {
                if (!strcmp(svp->sp[i].name, name))
                {
                    if (crackISState(pcdataXMLEle(ep), &svp->sp[i].s) < 0)
                    {
                        return (-1); /* unrecognized state */
                    }
                    break;
                }
            }
        }
    }

    /* ok */
    return (0);
}

/* crack the snooped driver setBLOBVector message into the given
 * IBLOBVectorProperty. it is not necessary that all IBLOB names be found.
 * return 0 if type, device and name match, else return -1.
 * N.B. we assume any existing blob in bvp has been malloced, which we free
 *   and replace with a newly malloced blob if found.
 */
int IUSnoopBLOB(XMLEle *root, IBLOBVectorProperty *bvp)
{
    char *dev, *name;
    XMLEle *ep;

    /* check and crack type, device, name and state */
    if (strcmp(tagXMLEle(root), "setBLOBVector") || crackDN(root, &dev, &name, NULL) < 0)
        return (-1);

    if (strcmp(dev, bvp->device) || strcmp(name, bvp->name))
        return (-1); /* not this property */

    crackIPState(findXMLAttValu(root, "state"), &bvp->s);

    for (ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
    {
        if (strcmp(tagXMLEle(ep), "oneBLOB") == 0)
        {
            XMLAtt *na = findXMLAtt(ep, "name");
            if (na == NULL)
                return (-1);

            IBLOB *bp = IUFindBLOB(bvp, valuXMLAtt(na));

            if (bp == NULL)
                return (-1);

            XMLAtt *fa = findXMLAtt(ep, "format");
            XMLAtt *sa = findXMLAtt(ep, "size");
            XMLAtt *ec = findXMLAtt(ep, "enclen");
            if (fa && sa && ec)
            {
                int enclen  = atoi(valuXMLAtt(ec));
                assert_mem(bp->blob = realloc(bp->blob, 3 * enclen / 4));
                bp->bloblen = from64tobits_fast(bp->blob, pcdataXMLEle(ep), enclen);
                strncpy(bp->format, valuXMLAtt(fa), MAXINDIFORMAT);
                bp->size = atoi(valuXMLAtt(sa));
            }
        }
    }

    /* ok */
    return (0);
}

/* callback when INDI client message arrives on stdin.
 * collect and dispatch when see outter element closure.
 * exit if OS trouble or see incompatable INDI version.
 * arg is not used.
 */
void clientMsgCB(int fd, void *arg)
{
    (void)arg;
    char buf[MAXRBUF], msg[MAXRBUF], *bp;
    int nr;

    /* one read */
    nr = read(fd, buf, sizeof(buf));
    if (nr < 0)
    {
        fprintf(stderr, "%s: %s\n", me, strerror(errno));
        exit(1);
    }
    if (nr == 0)
    {
        fprintf(stderr, "%s: EOF\n", me);
        exit(1);
    }

    /* crack and dispatch when complete */
    for (bp = buf; nr-- > 0; bp++)
    {
        XMLEle *root = readXMLEle(clixml, *bp, msg);
        if (root)
        {
            if (dispatch(root, msg) < 0)
                fprintf(stderr, "%s dispatch error: %s\n", me, msg);
            delXMLEle(root);
        }
        else if (msg[0])
            fprintf(stderr, "%s XML error: %s\n", me, msg);
    }
}

/* crack the given INDI XML element and call driver's IS* entry points as they
 *   are recognized.
 * return 0 if ok else -1 with reason in msg[].
 * N.B. exit if getProperties does not proclaim a compatible version.
 */
int dispatch(XMLEle *root, char msg[])
{
    char *rtag = tagXMLEle(root);
    XMLEle *ep;
    int n, i = 0;

    if (verbose)
        prXMLEle(stderr, root, 0);

    if (!strcmp(rtag, "getProperties"))
    {
        XMLAtt *ap, *name, *dev;
        double v;

        /* check version */
        ap = findXMLAtt(root, "version");
        if (!ap)
        {
            fprintf(stderr, "%s: getProperties missing version\n", me);
            exit(1);
        }
        v = atof(valuXMLAtt(ap));
        if (v > INDIV)
        {
            fprintf(stderr, "%s: client version %g > %g\n", me, v, INDIV);
            exit(1);
        }

        // Get device
        dev = findXMLAtt(root, "device");

        // Get property name
        name = findXMLAtt(root, "name");

        if (name && dev)
        {
            int index = isPropDefined(valuXMLAtt(name), valuXMLAtt(dev));
            if (index < 0)
                return 0;

            ROSC *prop = propCache + index;
            switch (prop->type)
            {
                /* JM 2019-07-18: Why are we using setXXX here? should be defXXX */
                case INDI_NUMBER:
                    //IDSetNumber((INumberVectorProperty *)(prop->ptr), NULL);
                    IDDefNumber((INumberVectorProperty *)(prop->ptr), NULL);
                    return 0;

                case INDI_SWITCH:
                    //IDSetSwitch((ISwitchVectorProperty *)(prop->ptr), NULL);
                    IDDefSwitch((ISwitchVectorProperty *)(prop->ptr), NULL);
                    return 0;

                case INDI_TEXT:
                    //IDSetText((ITextVectorProperty *)(prop->ptr), NULL);
                    IDDefText((ITextVectorProperty *)(prop->ptr), NULL);
                    return 0;

                case INDI_BLOB:
                    //IDSetBLOB((IBLOBVectorProperty *)(prop->ptr), NULL);
                    IDDefBLOB((IBLOBVectorProperty *)(prop->ptr), NULL);
                    return 0;
                default:
                    return 0;
            }
        }

        ISGetProperties(dev ? valuXMLAtt(dev) : NULL);
        return (0);
    }

    /* other commands might be from a snooped device.
         * we don't know here which devices are being snooped so we send
         * all remaining valid messages
         */
    if (!strcmp(rtag, "setNumberVector") || !strcmp(rtag, "setTextVector") || !strcmp(rtag, "setLightVector") ||
            !strcmp(rtag, "setSwitchVector") || !strcmp(rtag, "setBLOBVector") || !strcmp(rtag, "defNumberVector") ||
            !strcmp(rtag, "defTextVector") || !strcmp(rtag, "defLightVector") || !strcmp(rtag, "defSwitchVector") ||
            !strcmp(rtag, "defBLOBVector") || !strcmp(rtag, "message") || !strcmp(rtag, "delProperty"))
    {
        ISSnoopDevice(root);
        return (0);
    }

    char *dev, *name;
    /* pull out device and name */
    if (crackDN(root, &dev, &name, msg) < 0)
        return (-1);

    if (isPropDefined(name, dev) < 0)
    {
        snprintf(msg, MAXRBUF, "Property %s is not defined in %s.", name, dev);
        return -1;
    }

    /* ensure property is not RO */
    for (i = 0; i < nPropCache; i++)
    {
        if (!strcmp(propCache[i].propName, name) && !strcmp(propCache[i].devName, dev))
        {
            if (propCache[i].perm == IP_RO)
            {
                snprintf(msg, MAXRBUF, "Cannot set read-only property %s", name);
                return -1;
            }
            else
                break;
        }
    }

    /* check tag in surmised decreasing order of likelyhood */

    if (!strcmp(rtag, "newNumberVector"))
    {
        static double *doubles;
        static char **names;
        static int maxn;

        /* seed for reallocs */
        if (!doubles)
        {
            assert_mem(doubles = (double *)malloc(sizeof *doubles));
            assert_mem(names = (char **)malloc(sizeof *names));
        }

        // Set locale to C and save previous value
        locale_char_t *orig = indi_locale_C_numeric_push();

        /* pull out each name/value pair */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneNumber") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                if (na)
                {
                    if (n >= maxn)
                    {
                        /* grow for this and another */
                        maxn = n + 1;
                        assert_mem(doubles = (double *)realloc(doubles, maxn * sizeof *doubles));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                    }
                    if (f_scansexa(pcdataXMLEle(ep), &doubles[n]) < 0)
                        IDMessage(dev, "[ERROR] %s: Bad format %s", name, pcdataXMLEle(ep));
                    else
                        names[n++] = valuXMLAtt(na);
                }
            }
        }

        // Reset locale settings to original value
        indi_locale_C_numeric_pop(orig);

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
            ISNewNumber(dev, name, doubles, names, n);
        else
            IDMessage(dev, "[ERROR] %s: newNumberVector with no valid members", name);
        return (0);
    }

    if (!strcmp(rtag, "newSwitchVector"))
    {
        static ISState *states;
        static char **names;
        static int maxn;
        XMLEle *ep;

        /* seed for reallocs */
        if (!states)
        {
            assert_mem(states = (ISState *)malloc(sizeof *states));
            assert_mem(names  = (char **)malloc(sizeof *names));
        }

        /* pull out each name/state pair */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneSwitch") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                if (na)
                {
                    if (n >= maxn)
                    {
                        maxn = n + 1;
                        assert_mem(states = (ISState *)realloc(states, maxn * sizeof *states));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                    }
                    if (strncmp(pcdataXMLEle(ep), "On", 2) == 0)
                    {
                        states[n] = ISS_ON;
                        names[n]  = valuXMLAtt(na);
                        n++;
                    }
                    else if (strcmp(pcdataXMLEle(ep), "Off") == 0)
                    {
                        states[n] = ISS_OFF;
                        names[n]  = valuXMLAtt(na);
                        n++;
                    }
                    else
                        IDMessage(dev, "[ERROR] %s: must be On or Off: %s", name, pcdataXMLEle(ep));
                }
            }
        }

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
            ISNewSwitch(dev, name, states, names, n);
        else
            IDMessage(dev, "[ERROR] %s: newSwitchVector with no valid members", name);
        return (0);
    }

    if (!strcmp(rtag, "newTextVector"))
    {
        static char **texts;
        static char **names;
        static int maxn;

        /* seed for reallocs */
        if (!texts)
        {
            assert_mem(texts = (char **)malloc(sizeof *texts));
            assert_mem(names = (char **)malloc(sizeof *names));
        }

        /* pull out each name/text pair */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneText") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                if (na)
                {
                    if (n >= maxn)
                    {
                        maxn = n + 1;
                        assert_mem(texts = (char **)realloc(texts, maxn * sizeof *texts));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                    }
                    texts[n] = pcdataXMLEle(ep);
                    names[n] = valuXMLAtt(na);
                    n++;
                }
            }
        }

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
            ISNewText(dev, name, texts, names, n);
        else
            IDMessage(dev, "[ERROR] %s: set with no valid members", name);
        return (0);
    }

    if (!strcmp(rtag, "newBLOBVector"))
    {
        static char **blobs;
        static char **names;
        static char **formats;
        static int *blobsizes;
        static int *sizes;
        static int maxn;
        int i;

        /* seed for reallocs */
        if (!blobs)
        {
            assert_mem(blobs = (char **)malloc(sizeof *blobs));
            assert_mem(names = (char **)malloc(sizeof *names));
            assert_mem(formats = (char **)malloc(sizeof *formats));
            assert_mem(blobsizes = (int *)malloc(sizeof *blobsizes));
            assert_mem(sizes = (int *)malloc(sizeof *sizes));
        }

        /* pull out each name/BLOB pair, decode */
        for (n = 0, ep = nextXMLEle(root, 1); ep; ep = nextXMLEle(root, 0))
        {
            if (strcmp(tagXMLEle(ep), "oneBLOB") == 0)
            {
                XMLAtt *na = findXMLAtt(ep, "name");
                XMLAtt *fa = findXMLAtt(ep, "format");
                XMLAtt *sa = findXMLAtt(ep, "size");
                XMLAtt *el = findXMLAtt(ep, "enclen");
                if (na && fa && sa)
                {
                    if (n >= maxn)
                    {
                        maxn = n + 1;
                        assert_mem(blobs = (char **)realloc(blobs, maxn * sizeof *blobs));
                        assert_mem(names = (char **)realloc(names, maxn * sizeof *names));
                        assert_mem(formats = (char **)realloc(formats, maxn * sizeof *formats));
                        assert_mem(sizes = (int *)realloc(sizes, maxn * sizeof *sizes));
                        assert_mem(blobsizes = (int *)realloc(blobsizes, maxn * sizeof *blobsizes));
                    }
                    int bloblen = pcdatalenXMLEle(ep);
                    // enclen is optional and not required by INDI protocol
                    if (el)
                        bloblen = atoi(valuXMLAtt(el));
                    assert_mem(blobs[n] = malloc(3 * bloblen / 4));
                    blobsizes[n] = from64tobits_fast(blobs[n], pcdataXMLEle(ep), bloblen);
                    names[n]     = valuXMLAtt(na);
                    formats[n]   = valuXMLAtt(fa);
                    sizes[n]     = atoi(valuXMLAtt(sa));
                    n++;
                }
            }
        }

        /* invoke driver if something to do, but not an error if not */
        if (n > 0)
        {
            ISNewBLOB(dev, name, sizes, blobsizes, blobs, formats, names, n);
            for (i = 0; i < n; i++)
                free(blobs[i]);
        }
        else
            IDMessage(dev, "[ERROR] %s: newBLOBVector with no valid members", name);
        return (0);
    }

    sprintf(msg, "Unknown command: %s", rtag);
    return (1);
}

int IUReadConfig(const char *filename, const char *dev, const char *property, int silent, char errmsg[])
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    LilXML *lp = newLilXML();

    FILE *fp = IUGetConfigFP(filename, dev, "r", errmsg);

    if (fp == NULL)
        return -1;

    char whynot[MAXRBUF];
    fproot = readXMLFile(fp, lp, whynot);

    delLilXML(lp);

    if (fproot == NULL)
    {
        snprintf(errmsg, MAXRBUF, "Unable to parse config XML: %s", whynot);
        fclose(fp);
        return -1;
    }

    if (nXMLEle(fproot) > 0 && silent != 1)
        IDMessage(dev, "[INFO] Loading device configuration...");

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            dispatch(root, errmsg);
            if (property)
                break;
        }
    }

    if (nXMLEle(fproot) > 0 && silent != 1)
        IDMessage(dev, "[INFO] Device configuration applied.");

    fclose(fp);
    delXMLEle(fproot);

    return (0);
}

int IUSaveDefaultConfig(const char *source_config, const char *dest_config, const char *dev)
{
    char configFileName[MAXRBUF], configDefaultFileName[MAXRBUF];

    if (source_config)
        strncpy(configFileName, source_config, MAXRBUF);
    else
    {
        if (getenv("INDICONFIG"))
            strncpy(configFileName, getenv("INDICONFIG"), MAXRBUF);
        else
            snprintf(configFileName, MAXRBUF, "%s/.indi/%s_config.xml", getenv("HOME"), dev);
    }

    if (dest_config)
        strncpy(configDefaultFileName, dest_config, MAXRBUF);
    else if (getenv("INDICONFIG"))
        snprintf(configDefaultFileName, MAXRBUF, "%s.default", getenv("INDICONFIG"));
    else
        snprintf(configDefaultFileName, MAXRBUF, "%s/.indi/%s_config.xml.default", getenv("HOME"), dev);

    // If the default doesn't exist, create it.
    if (access(configDefaultFileName, F_OK))
    {
        FILE *fpin = fopen(configFileName, "r");
        if (fpin != NULL)
        {
            FILE *fpout = fopen(configDefaultFileName, "w");
            if (fpout != NULL)
            {
                int ch = 0;
                while ((ch = getc(fpin)) != EOF)
                    putc(ch, fpout);

                fflush(fpout);
                fclose(fpout);
            }
            fclose(fpin);

            return 0;
        }
    }
    // If default config file exists already, then no need to modify it
    else
        return 0;


    return -1;
}

int IUGetConfigOnSwitch(const ISwitchVectorProperty *property, int *index)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int propertyFound = 0;
    *index = -1;

    FILE *fp = IUGetConfigFP(NULL, property->device, "r", errmsg);

    if (fp == NULL)
        return -1;

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(property->device, rdev))
            continue;

        if (!strcmp(property->name, rname))
        {
            propertyFound = 1;
            XMLEle *oneSwitch = NULL;
            int oneSwitchIndex = 0;
            ISState oneSwitchState;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0), oneSwitchIndex++)
            {
                if (crackISState(pcdataXMLEle(oneSwitch), &oneSwitchState) == 0 && oneSwitchState == ISS_ON)
                {
                    *index = oneSwitchIndex;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (propertyFound ? 0 : -1);
}

int IUGetConfigSwitch(const char *dev, const char *property, const char *member, ISState *value)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
        return -1;

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneSwitch = NULL;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0))
            {
                if (!strcmp(member, findXMLAttValu(oneSwitch, "name")))
                {
                    if (crackISState(pcdataXMLEle(oneSwitch), value) == 0)
                        valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

int IUGetConfigOnSwitchIndex(const char *dev, const char *property, int *index)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    *index = -1;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
        return -1;

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneSwitch = NULL;
            int currentIndex = 0;
            for (oneSwitch = nextXMLEle(root, 1); oneSwitch != NULL; oneSwitch = nextXMLEle(root, 0), currentIndex++)
            {
                ISState s = ISS_OFF;
                if (crackISState(pcdataXMLEle(oneSwitch), &s) == 0 && s == ISS_ON)
                {
                    *index = currentIndex;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (*index >= 0 ? 0 : -1);
}

int IUGetConfigNumber(const char *dev, const char *property, const char *member, double *value)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
        return -1;

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneNumber = NULL;
            for (oneNumber = nextXMLEle(root, 1); oneNumber != NULL; oneNumber = nextXMLEle(root, 0))
            {
                if (!strcmp(member, findXMLAttValu(oneNumber, "name")))
                {
                    *value = atof(pcdataXMLEle(oneNumber));
                    valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

int IUGetConfigText(const char *dev, const char *property, const char *member, char *value, int len)
{
    char *rname, *rdev;
    XMLEle *root = NULL, *fproot = NULL;
    char errmsg[MAXRBUF];
    LilXML *lp = newLilXML();
    int valueFound = 0;

    FILE *fp = IUGetConfigFP(NULL, dev, "r", errmsg);

    if (fp == NULL)
        return -1;

    fproot = readXMLFile(fp, lp, errmsg);

    if (fproot == NULL)
    {
        fclose(fp);
        return -1;
    }

    for (root = nextXMLEle(fproot, 1); root != NULL; root = nextXMLEle(fproot, 0))
    {
        /* pull out device and name */
        if (crackDN(root, &rdev, &rname, errmsg) < 0)
        {
            fclose(fp);
            delXMLEle(fproot);
            return -1;
        }

        // It doesn't belong to our device??
        if (strcmp(dev, rdev))
            continue;

        if ((property && !strcmp(property, rname)) || property == NULL)
        {
            XMLEle *oneText = NULL;
            for (oneText = nextXMLEle(root, 1); oneText != NULL; oneText = nextXMLEle(root, 0))
            {
                if (!strcmp(member, findXMLAttValu(oneText, "name")))
                {
                    strncpy(value, pcdataXMLEle(oneText), len);
                    valueFound = 1;
                    break;
                }
            }
            break;
        }
    }

    fclose(fp);
    delXMLEle(fproot);
    delLilXML(lp);

    return (valueFound == 1 ? 0 : -1);
}

/* send client a message for a specific device or at large if !dev */
void IDMessageVA(const char *dev, const char *fmt, va_list ap)
{
    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    printf("<message\n");
    if (dev)
        printf(" device='%s'\n", dev);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        printf("  message='");
        escapeXML_fputs(message, stdout);
        printf("'\n");
    }
    printf("/>\n");
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}

void IDMessage(const char *dev, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDMessageVA(dev, fmt, ap);
    va_end(ap);
}

int IUPurgeConfig(const char *filename, const char *dev, char errmsg[])
{
    char configFileName[MAXRBUF];
    char configDir[MAXRBUF];

    snprintf(configDir, MAXRBUF, "%s/.indi/", getenv("HOME"));

    if (filename)
        strncpy(configFileName, filename, MAXRBUF);
    else
    {
        if (getenv("INDICONFIG"))
            strncpy(configFileName, getenv("INDICONFIG"), MAXRBUF);
        else
            snprintf(configFileName, MAXRBUF, "%s%s_config.xml", configDir, dev);
    }

    if (remove(configFileName) != 0)
    {
        snprintf(errmsg, MAXRBUF, "Unable to purge configuration file %s. Error %s", configFileName, strerror(errno));
        return -1;
    }

    return 0;
}

FILE *IUGetConfigFP(const char *filename, const char *dev, const char *mode, char errmsg[])
{
    char configFileName[MAXRBUF];
    char configDir[MAXRBUF];
    struct stat st;
    FILE *fp = NULL;

    snprintf(configDir, MAXRBUF, "%s/.indi/", getenv("HOME"));

    if (filename)
        strncpy(configFileName, filename, MAXRBUF);
    else
    {
        if (getenv("INDICONFIG"))
            strncpy(configFileName, getenv("INDICONFIG"), MAXRBUF);
        else
            snprintf(configFileName, MAXRBUF, "%s%s_config.xml", configDir, dev);
    }

    if (stat(configDir, &st) != 0)
    {
        if (mkdir(configDir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) < 0)
        {
            snprintf(errmsg, MAXRBUF, "Unable to create config directory. Error %s: %s", configDir, strerror(errno));
            return NULL;
        }
    }

    stat(configFileName, &st);
    /* If file is owned by root and current user is NOT root then abort */
    if ( (st.st_uid == 0 && getuid() != 0) || (st.st_gid == 0 && getgid() != 0) )
    {
        strncpy(errmsg,
                "Config file is owned by root! This will lead to serious errors. To fix this, run: sudo chown -R $USER:$USER ~/.indi",
                MAXRBUF);
        return NULL;
    }

    fp = fopen(configFileName, mode);
    if (fp == NULL)
    {
        snprintf(errmsg, MAXRBUF, "Unable to open config file. Error loading file %s: %s", configFileName,
                 strerror(errno));
        return NULL;
    }

    return fp;
}

void IUSaveConfigTag(FILE *fp, int ctag, const char *dev, int silent)
{
    if (!fp)
        return;

    /* Opening tag */
    if (ctag == 0)
    {
        fprintf(fp, "<INDIDriver>\n");
        if (silent != 1)
            IDMessage(dev, "[INFO] Saving device configuration...");
    }
    /* Closing tag */
    else
    {
        fprintf(fp, "</INDIDriver>\n");
        if (silent != 1)
            IDMessage(dev, "[INFO] Device configuration saved.");
    }
}

void IUSaveConfigNumber(FILE *fp, const INumberVectorProperty *nvp)
{
    int i;

    locale_char_t *orig = indi_locale_C_numeric_push();
    fprintf(fp, "<newNumberVector device='%s' name='%s'>\n", nvp->device, nvp->name);

    for (i = 0; i < nvp->nnp; i++)
    {
        INumber *np = &nvp->np[i];
        fprintf(fp, "  <oneNumber name='%s'>\n", np->name);
        fprintf(fp, "      %.20g\n", np->value);
        fprintf(fp, "  </oneNumber>\n");
    }

    fprintf(fp, "</newNumberVector>\n");
    indi_locale_C_numeric_pop(orig);
}

void IUSaveConfigText(FILE *fp, const ITextVectorProperty *tvp)
{
    int i;

    fprintf(fp, "<newTextVector device='%s' name='%s'>\n", tvp->device, tvp->name);

    for (i = 0; i < tvp->ntp; i++)
    {
        IText *tp = &tvp->tp[i];
        fprintf(fp, "  <oneText name='%s'>\n", tp->name);
        fprintf(fp, "      %s\n", tp->text ? tp->text : "");
        fprintf(fp, "  </oneText>\n");
    }

    fprintf(fp, "</newTextVector>\n");
}

void IUSaveConfigSwitch(FILE *fp, const ISwitchVectorProperty *svp)
{
    int i;

    fprintf(fp, "<newSwitchVector device='%s' name='%s'>\n", svp->device, svp->name);

    for (i = 0; i < svp->nsp; i++)
    {
        ISwitch *sp = &svp->sp[i];
        fprintf(fp, "  <oneSwitch name='%s'>\n", sp->name);
        fprintf(fp, "      %s\n", sstateStr(sp->s));
        fprintf(fp, "  </oneSwitch>\n");
    }

    fprintf(fp, "</newSwitchVector>\n");
}

void IUSaveConfigBLOB(FILE *fp, const IBLOBVectorProperty *bvp)
{
    int i;

    fprintf(fp, "<newBLOBVector device='%s' name='%s'>\n", bvp->device, bvp->name);

    for (i = 0; i < bvp->nbp; i++)
    {
        IBLOB *bp = &bvp->bp[i];
        unsigned char *encblob = NULL;
        int l = 0;

        fprintf(fp, "  <oneBLOB\n");
        fprintf(fp, "    name='%s'\n", bp->name);
        fprintf(fp, "    size='%d'\n", bp->size);
        fprintf(fp, "    format='%s'>\n", bp->format);

        assert_mem(encblob = malloc(4 * bp->bloblen / 3 + 4));
        l = to64frombits_s(encblob, bp->blob, bp->bloblen, bp->bloblen);
        if (l == 0) {
            fprintf(stderr, "%s(%s): Not enough memory for decoding.\n", me, __func__);
            exit(1);
        }
        size_t written = 0;

        while ((int)written < l)
        {
            size_t towrite = ((l - written) > 72) ? 72 : l - written;
            size_t wr      = fwrite(encblob + written, 1, towrite, fp);

            fputc('\n', fp);
            if (wr > 0)
                written += wr;
        }
        free(encblob);

        fprintf(fp, "  </oneBLOB>\n");
    }

    fprintf(fp, "</newBLOBVector>\n");
}

/* tell client to create a text vector property */
void IDDefTextVA(const ITextVectorProperty *tvp, const char *fmt, va_list ap)
{
    int i;
    ROSC *SC;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<defTextVector\n");
    printf("  device='%s'\n", tvp->device);
    printf("  name='%s'\n", tvp->name);
    printf("  label='%s'\n", tvp->label);
    printf("  group='%s'\n", tvp->group);
    printf("  state='%s'\n", pstateStr(tvp->s));
    printf("  perm='%s'\n", permStr(tvp->p));
    printf("  timeout='%g'\n", tvp->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        printf("  message='");
        escapeXML_fputs(message, stdout);
        printf("'\n");
    }
    printf(">\n");

    for (i = 0; i < tvp->ntp; i++)
    {
        IText *tp = &tvp->tp[i];
        printf("  <defText\n");
        printf("    name='%s'\n", tp->name);
        printf("    label='%s'>\n", tp->label);
        printf("      %s\n", tp->text ? tp->text : "");
        printf("  </defText>\n");
    }

    printf("</defTextVector>\n");

    if (isPropDefined(tvp->name, tvp->device) < 0)
    {
        /* Add this property to insure proper sanity check */
        assert_mem(propCache = (ROSC *)(propCache ? realloc(propCache, (nPropCache + 1) * sizeof *propCache) : malloc(sizeof *propCache)));
        SC = &propCache[nPropCache++];

        strcpy(SC->propName, tvp->name);
        strcpy(SC->devName, tvp->device);
        SC->perm = tvp->p;
        SC->ptr  = tvp;
        SC->type = INDI_TEXT;
    }

    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}

void IDDefText(const ITextVectorProperty *tvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefTextVA(tvp, fmt, ap);
    va_end(ap);
}

/* tell client to create a new numeric vector property */
void IDDefNumberVA(const INumberVectorProperty *n, const char *fmt, va_list ap)
{
    int i;
    ROSC *SC;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<defNumberVector\n");
    printf("  device='%s'\n", n->device);
    printf("  name='%s'\n", n->name);
    printf("  label='%s'\n", n->label);
    printf("  group='%s'\n", n->group);
    printf("  state='%s'\n", pstateStr(n->s));
    printf("  perm='%s'\n", permStr(n->p));
    printf("  timeout='%g'\n", n->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        printf("  message='");
        escapeXML_fputs(message, stdout);
        printf("'\n");
    }
    printf(">\n");

    for (i = 0; i < n->nnp; i++)
    {
        INumber *np = &n->np[i];

        printf("  <defNumber\n");
        printf("    name='%s'\n", np->name);
        printf("    label='%s'\n", np->label);
        printf("    format='%s'\n", np->format);
        printf("    min='%.20g'\n", np->min);
        printf("    max='%.20g'\n", np->max);
        printf("    step='%.20g'>\n", np->step);
        printf("      %.20g\n", np->value);

        printf("  </defNumber>\n");
    }

    printf("</defNumberVector>\n");

    if (isPropDefined(n->name, n->device) < 0)
    {
        /* Add this property to insure proper sanity check */
        assert_mem(propCache = (ROSC *) (propCache ? realloc(propCache, (nPropCache + 1) * sizeof *propCache) : malloc(sizeof *propCache)));
        SC = &propCache[nPropCache++];

        strcpy(SC->propName, n->name);
        strcpy(SC->devName, n->device);
        SC->perm = n->p;
        SC->ptr  = n;
        SC->type = INDI_NUMBER;
    }

    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}

void IDDefNumber(const INumberVectorProperty *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefNumberVA(n, fmt, ap);
    va_end(ap);
}

/* tell client to create a new switch vector property */
void IDDefSwitchVA(const ISwitchVectorProperty *s, const char *fmt, va_list ap)

{
    int i;
    ROSC *SC;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<defSwitchVector\n");
    printf("  device='%s'\n", s->device);
    printf("  name='%s'\n", s->name);
    printf("  label='%s'\n", s->label);
    printf("  group='%s'\n", s->group);
    printf("  state='%s'\n", pstateStr(s->s));
    printf("  perm='%s'\n", permStr(s->p));
    printf("  rule='%s'\n", ruleStr(s->r));
    printf("  timeout='%g'\n", s->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        printf("  message='");
        escapeXML_fputs(message, stdout);
        printf("'\n");
    }
    printf(">\n");

    for (i = 0; i < s->nsp; i++)
    {
        ISwitch *sp = &s->sp[i];
        printf("  <defSwitch\n");
        printf("    name='%s'\n", sp->name);
        printf("    label='%s'>\n", sp->label);
        printf("      %s\n", sstateStr(sp->s));
        printf("  </defSwitch>\n");
    }

    printf("</defSwitchVector>\n");

    if (isPropDefined(s->name, s->device) < 0)
    {
        /* Add this property to insure proper sanity check */
        assert_mem(propCache = (ROSC *) (propCache ? realloc(propCache, (nPropCache + 1) * sizeof *propCache) : malloc(sizeof *propCache)));
        SC = &propCache[nPropCache++];

        strcpy(SC->propName, s->name);
        strcpy(SC->devName, s->device);
        SC->perm = s->p;
        SC->ptr  = s;
        SC->type = INDI_SWITCH;
    }

    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}

void IDDefSwitch(const ISwitchVectorProperty *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefSwitchVA(s, fmt, ap);
    va_end(ap);
}

/* tell client to create a new lights vector property */
void IDDefLightVA(const ILightVectorProperty *lvp, const char *fmt, va_list ap)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    printf("<defLightVector\n");
    printf("  device='%s'\n", lvp->device);
    printf("  name='%s'\n", lvp->name);
    printf("  label='%s'\n", lvp->label);
    printf("  group='%s'\n", lvp->group);
    printf("  state='%s'\n", pstateStr(lvp->s));
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        printf("  message='");
        escapeXML_fputs(message, stdout);
        printf("'\n");
    }
    printf(">\n");

    for (i = 0; i < lvp->nlp; i++)
    {
        ILight *lp = &lvp->lp[i];
        printf("  <defLight\n");
        printf("    name='%s'\n", lp->name);
        printf("    label='%s'>\n", lp->label);
        printf("      %s\n", pstateStr(lp->s));
        printf("  </defLight>\n");
    }

    printf("</defLightVector>\n");
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}

void IDDefLight(const ILightVectorProperty *lvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefLightVA(lvp, fmt, ap);
    va_end(ap);
}

/* tell client to create a new BLOB vector property */
void IDDefBLOBVA(const IBLOBVectorProperty *b, const char *fmt, va_list ap)
{
    int i;
    ROSC *SC;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<defBLOBVector\n");
    printf("  device='%s'\n", b->device);
    printf("  name='%s'\n", b->name);
    printf("  label='%s'\n", b->label);
    printf("  group='%s'\n", b->group);
    printf("  state='%s'\n", pstateStr(b->s));
    printf("  perm='%s'\n", permStr(b->p));
    printf("  timeout='%g'\n", b->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        printf("  message='");
        escapeXML_fputs(message, stdout);
        printf("'\n");
    }
    printf(">\n");

    for (i = 0; i < b->nbp; i++)
    {
        IBLOB *bp = &b->bp[i];
        printf("  <defBLOB\n");
        printf("    name='%s'\n", bp->name);
        printf("    label='%s'\n", bp->label);
        printf("  />\n");
    }

    printf("</defBLOBVector>\n");

    if (isPropDefined(b->name, b->device) < 0)
    {
        /* Add this property to insure proper sanity check */
        assert_mem(propCache = (ROSC *)(propCache ? realloc(propCache, (nPropCache + 1) * sizeof *propCache) : malloc(sizeof *propCache)));
        SC = &propCache[nPropCache++];

        strcpy(SC->propName, b->name);
        strcpy(SC->devName, b->device);
        SC->perm = b->p;
        SC->ptr  = b;
        SC->type = INDI_BLOB;
    }

    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}
void IDDefBLOB(const IBLOBVectorProperty *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDDefBLOBVA(b, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing text vector property */
void IDSetTextVA(const ITextVectorProperty *tvp, const char *fmt, va_list ap)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<setTextVector\n");
    printf("  device='%s'\n", tvp->device);
    printf("  name='%s'\n", tvp->name);
    printf("  state='%s'\n", pstateStr(tvp->s));
    printf("  timeout='%g'\n", tvp->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        // #PS: why not escapeXML_fputs?
        printf("  message='");
        printf("%s'\n", entityXML(message));
    }
    printf(">\n");

    for (i = 0; i < tvp->ntp; i++)
    {
        IText *tp = &tvp->tp[i];
        printf("  <oneText name='%s'>\n", tp->name);
        printf("      %s\n", tp->text ? entityXML(tp->text) : "");
        printf("  </oneText>\n");
    }

    printf("</setTextVector>\n");
    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}
void IDSetText(const ITextVectorProperty *tvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetTextVA(tvp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing numeric vector property */
void IDSetNumberVA(const INumberVectorProperty *nvp, const char *fmt, va_list ap)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<setNumberVector\n");
    printf("  device='%s'\n", nvp->device);
    printf("  name='%s'\n", nvp->name);
    printf("  state='%s'\n", pstateStr(nvp->s));
    printf("  timeout='%g'\n", nvp->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        // #PS: why not escapeXML_fputs?
        printf("  message='");
        printf("%s'\n", entityXML(message));
    }
    printf(">\n");

    for (i = 0; i < nvp->nnp; i++)
    {
        INumber *np = &nvp->np[i];
        printf("  <oneNumber name='%s'>\n", np->name);
        printf("      %.20g\n", np->value);
        printf("  </oneNumber>\n");
    }

    printf("</setNumberVector>\n");
    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}
void IDSetNumber(const INumberVectorProperty *nvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetNumberVA(nvp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing switch vector property */
void IDSetSwitchVA(const ISwitchVectorProperty *svp, const char *fmt, va_list ap)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<setSwitchVector\n");
    printf("  device='%s'\n", svp->device);
    printf("  name='%s'\n", svp->name);
    printf("  state='%s'\n", pstateStr(svp->s));
    printf("  timeout='%g'\n", svp->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        // #PS: why not escapeXML_fputs?
        printf("  message='");
        printf("%s'\n", entityXML(message));
    }
    printf(">\n");

    for (i = 0; i < svp->nsp; i++)
    {
        ISwitch *sp = &svp->sp[i];
        printf("  <oneSwitch name='%s'>\n", sp->name);
        printf("      %s\n", sstateStr(sp->s));
        printf("  </oneSwitch>\n");
    }

    printf("</setSwitchVector>\n");
    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}
void IDSetSwitch(const ISwitchVectorProperty *svp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetSwitchVA(svp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing lights vector property */
void IDSetLightVA(const ILightVectorProperty *lvp, const char *fmt, va_list ap)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    printf("<setLightVector\n");
    printf("  device='%s'\n", lvp->device);
    printf("  name='%s'\n", lvp->name);
    printf("  state='%s'\n", pstateStr(lvp->s));
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        char message[MAXINDIMESSAGE];
        vsnprintf(message, MAXINDIMESSAGE, fmt, ap);

        // #PS: why not escapeXML_fputs?
        printf("  message='");
        printf("%s'\n", entityXML(message));
    }
    printf(">\n");

    for (i = 0; i < lvp->nlp; i++)
    {
        ILight *lp = &lvp->lp[i];
        printf("  <oneLight name='%s'>\n", lp->name);
        printf("      %s\n", pstateStr(lp->s));
        printf("  </oneLight>\n");
    }

    printf("</setLightVector>\n");
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}
void IDSetLight(const ILightVectorProperty *lvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetLightVA(lvp, fmt, ap);
    va_end(ap);
}

/* tell client to update an existing BLOB vector property */
void IDSetBLOBVA(const IBLOBVectorProperty *bvp, const char *fmt, va_list ap)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);

    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<setBLOBVector\n");
    printf("  device='%s'\n", bvp->device);
    printf("  name='%s'\n", bvp->name);
    printf("  state='%s'\n", pstateStr(bvp->s));
    printf("  timeout='%g'\n", bvp->timeout);
    printf("  timestamp='%s'\n", timestamp());
    if (fmt)
    {
        printf("  message='");
        // #PS: missing escapeXML_fputs ?
        vprintf(fmt, ap);
        printf("'\n");
    }
    printf(">\n");

    for (i = 0; i < bvp->nbp; i++)
    {
        IBLOB *bp = &bvp->bp[i];
        unsigned char *encblob;
        int l;

        printf("  <oneBLOB\n");
        printf("    name='%s'\n", bp->name);
        printf("    size='%d'\n", bp->size);

        // If size is zero, we are only sending a state-change
        if (bp->size == 0)
        {
            printf("    enclen='0'\n");
            printf("    format='%s'>\n", bp->format);
        }
        else
        {
            size_t sz = 4 * bp->bloblen / 3 + 4;
            assert_mem(encblob = malloc(sz));
            l = to64frombits_s(encblob, bp->blob, bp->bloblen, sz);
            if (l == 0) {
                fprintf(stderr, "%s(%s): Not enough memory for decoding.\n", me, __func__);
                exit(1);
            }
            printf("    enclen='%d'\n", l);
            printf("    format='%s'>\n", bp->format);
            size_t written = 0;

            while ((int)written < l)
            {
                size_t towrite = ((l - written) > 72) ? 72 : l - written;
                size_t wr      = fwrite(encblob + written, 1, towrite, stdout);

                if (wr > 0)
                    written += wr;
                if ((written % 72) == 0)
                    fputc('\n', stdout);
            }

            if ((written % 72) != 0)
                fputc('\n', stdout);

            free(encblob);
        }

        printf("  </oneBLOB>\n");
    }

    printf("</setBLOBVector>\n");
    indi_locale_C_numeric_pop(orig);
    fflush(stdout);

    pthread_mutex_unlock(&stdout_mutex);
}
void IDSetBLOB(const IBLOBVectorProperty *bvp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    IDSetBLOBVA(bvp, fmt, ap);
    va_end(ap);
}

/* tell client to update min/max elements of an existing number vector property */
void IUUpdateMinMax(const INumberVectorProperty *nvp)
{
    int i;

    pthread_mutex_lock(&stdout_mutex);
    xmlv1();
    locale_char_t *orig = indi_locale_C_numeric_push();
    printf("<setNumberVector\n");
    printf("  device='%s'\n", nvp->device);
    printf("  name='%s'\n", nvp->name);
    printf("  state='%s'\n", pstateStr(nvp->s));
    printf("  timeout='%g'\n", nvp->timeout);
    printf("  timestamp='%s'\n", timestamp());
    printf(">\n");

    for (i = 0; i < nvp->nnp; i++)
    {
        INumber *np = &nvp->np[i];
        printf("  <oneNumber name='%s'\n", np->name);
        printf("    min='%g'\n", np->min);
        printf("    max='%g'\n", np->max);
        printf("    step='%g'\n", np->step);
        printf(">\n");
        printf("      %g\n", np->value);
        printf("  </oneNumber>\n");
    }

    printf("</setNumberVector>\n");
    indi_locale_C_numeric_pop(orig);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);
}

int IUFindIndex(const char *needle, char **hay, unsigned int n)
{
    int i = 0;

    for (i = 0; i < (int)n; i++)
    {
        if (!strcmp(hay[i], needle))
            return i;
    }
    return -1;
}
