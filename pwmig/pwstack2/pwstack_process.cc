#include <math.h>
#include <cstdio>
#include <vector>
#include <sstream>
#include <algorithm>
#include "perf.h"
#include "coords.h"
#include "pwstack.h"
#include "seispp.h"

void dzero(int n, double *d, int inc)
{
    int i, ii;
    for(i=0,ii=0;i<n;++i,ii+=inc) d[ii]=0.0;
}


void vscal(int n, double *w, int incw, double *y, int incy)
{
    int i, iw, iy;
    for(i=0,iw=0,iy=0;i<n;++i,iw+=incw,iy+=incy)
        y[iy] = w[iw]*y[iy];
}


void vadd(int n, double *x,int  incx, double *y,int  incy)
{
    int i, ix, iy;
    for(i=0,ix=0,iy=0;i<n;++i,ix+=incx,iy+=incy)
        y[iy] = x[ix] + y[iy];
}


string virtual_station_name(int ix1, int ix2)
{
    char name[8];
    sprintf(name,"%03.3d%03.3d",ix1,ix2);
    return(string(name));
}


// make station name for pwstack using 3 integer ids
string MakeDfileName(int evid, int x1, int x2)
{
    string dfile;
    ostringstream sbuf(dfile);
    sbuf<<"pwstack_"<<evid<<"_"<<x1<<"_"<<x2;
    return(sbuf.str());
}


/*
Main processing function for this program.  Takes an input
data ensemble and produce a complete suite of plane-wave stacks
defined by the Rectangualr_Slowness_Grid object.
This function uses and enhancement from the original Neal and Pavlis
and Poppeliers and Pavlis papers.  It allows the aperture to
be tim variable.  This is generalized, but the normal expectation
is that the apeture would grow wider with time to compensate
somewhat for diffraction.

Arguments
indata - input raw data ensemble (see object definition
the enscapsulates all this requires)
ugrid - object to define slowness grid for stacking
mute - mute applied to data before stacking
stackmute - mute applied to data after stacking
(This stack is aligned relative to latest
mute time of raw data in stack.)
lat0, lon0 - pseudostation grid point (IN RADIANS)
ux0, uy0 - slowness vector of input data (stacking
is relative to this vector)
tstart:tend - define time period for output stack
The routine pretty much assumes relative timing
so this is normally time wrt the start of
each trace in the ensemble.
aperture - defines variable aperture stack weighting
(see above)
dtcoh - sample interval for coherence calculation
Assumed implicitly to be larger than data dt.
overlap - overlap fraction.  coherence windows at dtcoh
intervals overlpa by this fraction to form an
implicit smoothing.
mdlcopy - defines metadata to copy from the raw data
ensemble to each stack output
mdlout - list of metadata to be saved to output database
(fixed for a given schema)
am - AttributeMap object defining mapping of internal to
external namespace (invariant between calls)
dir and dfile - define file name where output written (dir/dfile)
dbh - DatabaseHandle object for output

Normal return is 0.  Returns nonzero if stack was null.
(This is not an exception as it will happen outside the boundaries
of an array and this case needs to be handled cleanly.

Changed July 1, 2008
Now returns count of fold for this grid point.  negative or 0
means did nothing.

Throws a MetadataError exception object if there are problems
parsing required metadata from any trace.  Current caller will
abort the program on this condition, but evolution might want
to produce a handler.
Change Jan 15,2004
Used to pass lat0, lon0 by arg list, now passed through the
ensemble metadata.
*/
int pwstack_ensemble(ThreeComponentEnsemble& indata,
RectangularSlownessGrid& ugrid,
TopMute& mute,
TopMute& stackmute,
int stack_count_cutoff,
double tstart,
double tend,
DepthDependentAperture& aperture,
double dtcoh,
double cohwinlen,
MetadataList& mdlcopy,
PwmigFileHandle& dfh,
PwmigFileHandle& coh3cfh,
PwmigFileHandle& cohfh)
{
    // lat0 and lon0 are location of target pseudostation grid point
    // elev0 is elevation of datum to use for geometric statics
    //
    double lat0,lon0,elev0;
    // Incident wavefield slowness vector components
    double ux0,uy0;
    string gridname;
    int ix1, ix2;
    int evid;
    string dfile;
    const double WEIGHT_MINIMUM=1.0e-2;
//DEBUG remove this fixed array buffer when completed
/*
double DEBUGBUFFER[10000];
vector<double>::iterator idbg;
*/
    try
    {
        lat0=indata.get_double("lat0");
        lon0=indata.get_double("lon0");
        elev0=indata.get_double("elev0");
        ux0=indata.get_double("ux0");
        uy0=indata.get_double("uy0");
        //DEBUG
        //cerr << "(ux0,uy0)=("<<ux0<<","<<uy0<<"("<<endl;
        ix1=indata.get_int("ix1");
        ix2=indata.get_int("ix2");
        evid=indata.get_int("evid");
        gridname=indata.get_string("gridname");
    } catch (MetadataGetError& mderr)
    {
        // The above are all set by main so this is the
        // correct error message.  Could perhaps be dropped
        // after debugging finished.
        mderr.log_error();
        cerr << "Coding error requiring a bug fix"<<endl;
        exit(-1);
    }
    // This routine builds a sta name from the index positions.
    // Made a function to allow consistency between programs.
    //
    string sta=virtual_station_name(ix1,ix2);
    int i,j,k;
    vector<ThreeComponentSeismogram>::iterator iv,ov;
    int nsta = indata.member.size();
    // This computes the output gather size.  It assumes all data have
    // a common sample rate and we can just grab the first one in the
    // list.  It also quietly assumes a relative time base
    // so all times are computed relative to the start of
    // each trace.  Caller should guarantee this.
    //
    double dt=indata.member[0].dt;
    int nsin = indata.member[0].ns;

    ThreeComponentSeismogram *stackout;

    int istart = SEISPP::nint(tstart/dt);
    int iend = SEISPP::nint(tend/dt);
    int nsout = iend-istart+1;
    int ismute = SEISPP::nint(mute.t1/dt);
    int ismute_this, ie_this;

    if(istart>=indata.member[0].ns)
        elog_die(0,(char *)"Irreconcilable window request:  Requested stack time window = %lf to %lf\nThis is outside range of input data\n",
            tstart,tend);

    /* Apply front end mutes to all traces */
    ApplyTopMute(indata,mute);

    /* We need dnorth, deast vectors to compute moveout sensibly
    for this program.  Since we use them repeatedly we will
    extract them once from the gather.*/
    vector <double> dnorth;
    vector <double> deast;
    vector <double> elev;
    dnorth.resize(nsta);   deast.resize(nsta);  elev.resize(nsta);
    for(i=0,iv=indata.member.begin();iv!=indata.member.end();++iv,++i)
    {
        double lat,lon;
        int ierr;
        try
        {
            lat = (*iv).get_double("site.lat");
            lon = (*iv).get_double("site.lon");
            // assume metadata store these in degrees so we have to convert
            lat = rad(lat);
            lon = rad(lon);
            geographic_to_dne(lat0, lon0, lat, lon, &(dnorth[i]),&(deast[i]));
            elev[i]=(*iv).get_double("site.elev");
            //DEBUG
            /*  This found no problems.  Working correctly and using radians consistently.
            January 6, 2007
            cerr << lat << " "
                << lat0 << " "
                << dnorth[i] << " "
                << lon << " "
                << lon0 << " "
                << deast[i] << endl;
            */
        } catch (MetadataError& merr)
        {
            throw merr;
        }
    }
    //
    // We want to make sure that all stations are in standard coordinates
    //
    for(iv=indata.member.begin();iv!=indata.member.end();++iv)
    {
        (*iv).rotate_to_standard();
    }
    //
    // the weights become are a nsta by nsamp matrix to allow
    // variable length apertures.  This algorithm assumes the
    // aperture object input is an nsta by
    // Should use a matrix class for this, but it is easy enough
    // for this simple case to just code inline.

    dmatrix weights(nsta,nsout);
    int stack_count=0;                            //maximum number of traces not zeroed
    vector <double> work(nsta);
    vector <bool> use_this_sta(nsta);
    for(i=0;i<nsta;++i) use_this_sta[i]=false;
    for(i=0;i<nsout;++i)
    {
        int nused;

        // Assume compute_pseudostation sets weights to zero outside
        // cutoff and not just skip them.  Otherwise we need an
        // intializer here.
        nused=compute_pseudostation_weights(nsta, &(dnorth[0]),&(deast[0]),
            aperture.get_aperture(tstart+dt*(double)i),
            aperture.get_cutoff(tstart+dt*(double)i),&(work[0]));
        for(j=0;j<nsta;++j)
        {
            if(work[j]>WEIGHT_MINIMUM)
            {
                weights(j,i)=work[j];
                if(!use_this_sta[j])use_this_sta[j]=true;
            }
            else
                weights(j,i)=0.0;
        }
        //stack_count=max(nused,stack_count);
    }
    //cerr<< "stack_count from nused="<<stack_count<<endl;
    for(i=0,stack_count=0;i<nsta;++i)
        if(use_this_sta[i])++stack_count;
    //cerr<< "stack count from new boolean vector="<<stack_count<<endl;
    ///
    // ERROR RETURN THAT NEEDS TO BE HANDLED GRACEFULLY
    // I don't throw an exception because this should not be viewed as
    // an exception.  It is a case that has to be handled gracefully.
    // it will happen often at the edges of arrays
    //
    if(stack_count<stack_count_cutoff) return(-1);
    cout << "Processing data for node ("<<ix1<<", "<<ix2<<") with fold="<<stack_count<<endl;
    vector <double>moveout(nsta);
    dmatrix stack(3,nsout);
    vector<double>stack_weight(nsout);
    vector<double>twork(nsout);

    double avg_elev,sum_wgt;                      // computed now as weighted sum of station elevations
    for(i=0,avg_elev=0.0,sum_wgt=0.0;i<nsta;++i)
    {
        avg_elev += weights(i,0)*elev[i];
        sum_wgt += weights(i,0);
    }
    if(sum_wgt<WEIGHT_MINIMUM) sum_wgt=WEIGHT_MINIMUM;
    //DEBUG
    //cerr << "sum_wgt="<<sum_wgt<<endl;
    avg_elev /= sum_wgt;
    // New March 2007:  these matrices hold stack members
    // and associated weights for each sample.
    // They are used for coherence calculations
    vector<dmatrix> gather;
    dmatrix gathwgt(nsout,stack_count);
    gathwgt.zero();
    /* Here we compute a vector of sum of weights at each time step 
    and copy only nonzero weight vectors to gathwgt vector.  We do
    this here because this vector and matrix  are common to all slowness vectors
    results we compute here. */
    int icol; // defined here and used in a similar context inside slowness grid loop.
    for(i=0,icol=0;i<nsta;++i)
    {
	if(use_this_sta[i])
	{
            vadd(nsout,weights.get_address(i,0),nsta,&(stack_weight[0]),1);
            dcopy(nsout,weights.get_address(i,0),nsta,
                            gathwgt.get_address(0,icol),1);
	    ++icol;
	}
    }
   /* Find the first sample with a sum of wts above the threshold */
    int stack_start;
    for(i=0;i<nsout;++i) 
    {
	if(stack_weight[i]>WEIGHT_MINIMUM)
        {
		stack_start=i;
		break;
	}
    }
    /* no reason to continue if all the weights are tiny as that means
    all coverage is at the fringe of the aperture.  Normally best to drop
    such points.  Return 0 in this case instead of -1 as above. */
    if(stack_start>=nsout) return(0);
    /* Apply the top mute to the start the data to taper discontinuties
    that can happen otherwise when the aperture gets wider with time. */
    TopMute smuteused(stackmute);
    if(stack_start>=0)
    {
	double tshift=dt*static_cast<double>(stack_start);
	smuteused.t0e+=tshift;;
	smuteused.t1+=tshift;
    }

    /* Create the working gather build as 3 matrices, one for each channel,
    with stack_count columns per member.   More logical than a 3d array 
    in this case in the authors view. */
    for(i=0;i<3;++i)
    {
        gather.push_back(dmatrix(nsout,stack_count));
    }
    //
    // These are buffers used to store results for each pseudostation point
    // to allow i/o to be concentrated.  This was causing a performance problem
    // this is aimd to address.
    //
    list<ThreeComponentSeismogram> stacklist;
    list <Coharray> coharraylist;
    //
    // Loop over slowness grid range storing results in new output ensemble
    //

    int iu,ju,gridid;
    double ux,uy,dux,duy;
    for(iu=0,gridid=1;iu<ugrid.nux;++iu)
    {
        for(ju=0;ju<ugrid.nuy;++ju,++gridid)
        {
            int iend_this;
            int ismin;
            char buffer[128];

            stack.zero();
            for(i=0;i<3;++i)gather[i].zero();

            dzero(nsout,&(stack_weight[0]),1);

            /* The input gather is assumed prealigned with the slowness
            vector defined by ux0,uy0.  We use relative moveouts
            from this base moveout for the actual stacks dux,duy*/
            dux = ugrid.ux(iu);
            duy = ugrid.uy(ju);

            ux =  ux0+dux;
            uy =  uy0+duy;
            // moveout computed here and used below assumes the
            // data are aligned on the P arrival
            compute_pwmoveout(nsta,&(deast[0]),&(dnorth[0]),dux,duy,&(moveout[0]));
            for(i=0,icol=0,iv=indata.member.begin(),ismute_this=0,iend_this=0,ismin=nsout;
                iv!=indata.member.end();++iv,++i)
            {
                int is0,ietest, is, ns_to_copy;
                int j0;
                double lag;
                nsin=iv->ns;
                //
                // Completely drop data for stations not marked with tiny or zero weight
                //
                if(use_this_sta[i])
                {

                    lag = tstart - (iv->t0) + moveout[i];
                    //DEBUG
                    /*
                    cerr << "lag="<<lag<<" time 0 lag="<<tstart - (iv->t0)<< "moveout="<<moveout[i]<<endl
                        << "i="<<i<<", dnorth="<<dnorth[i]<<", deast="<<deast[i]<<endl;
                    */
                    is0=SEISPP::nint(lag/dt);
                    if(is0>=0)
                    {
                        // This block is for positive moveout = negative shift
                        j0=0;
                        is=is0;
                        ismin=min(is,ismin);
                        ietest=is0+nsout-1;
                        if(ietest<=nsin)
                        {
                            ns_to_copy=nsout;
                            iend_this=nsout;
                        }
                        else
                        {
                            ns_to_copy=nsin-is0;
                            iend_this=max(ns_to_copy,iend_this);
                        }
                    }
                    else
                    {
                        // This block is for negative moveout=positive shift
                        j0=-is0;                  // j0 always positive here
                        is=0;
                        ismin=0;
                        ns_to_copy=nsout-j0;
                        if(ns_to_copy>nsin)
                        {
                            ns_to_copy=nsin-j0;
                            iend_this=max(nsin,iend_this);
                        }
                        else if(ns_to_copy<=0)
                        {
                            ns_to_copy=0;
                        }
                        else
                        {
                            iend_this=nsout;
                        }
                    }
                    //DEBUG
                    //cerr << "is="<<is<<" j0="<<j0<<"ns_to_copy="<<ns_to_copy<<" nsout="<<nsout<<endl;

                    if(ns_to_copy>0)
                    {
                        for(j=0;j<3;++j)
                        {
                            dzero(nsout,&(twork[0]),1);
                            // This is a slow way to copy these
                            // data to twork, but it is safe.
                            // An earlier version had a problem
                            // with stray indices.
                            int kk,jj;
                            for(k=0,kk=is,jj=j0;
                                (k<ns_to_copy)&&(kk<nsin)&&(jj<nsout);
                                ++k,++jj,++kk)
                            {
                                twork[jj]=iv->u(j,kk);
                                gather[j](jj,icol)=twork[jj];
                            }

//DEBUG
/*
dcopy(nsout,&(twork[0]),1,DEBUGBUFFER,1);
idbg=max_element(twork.begin(),twork.end());
cout << "Max before weighting="<<*idbg<<endl;
*/
                            //mp.load(twork,string("d0"));
                            vscal(nsout,weights.get_address(i,0),nsta,&(twork[0]),1);
                            //mp.load(twork,string("ds"));
//DEBUG
/*
dcopy(nsout,&(twork[0]),1,DEBUGBUFFER,1);
idbg=max_element(twork.begin(),twork.end());
cout << "Max after weighting="<<*idbg<<endl;
*/
                            vadd(nsout,&(twork[0]),1,stack.get_address(j,0),3);
                            //mp.load(stack,string("stack"));
			    /* We accumulate the stack_weight vector only on the first component. */
			    if(j==0) vadd(nsout,weights.get_address(i,0),nsta,&(stack_weight[0]),1);
#ifdef MATLABDEBUG
			    if(j==2)
			    {
                            mp.load(stack_weight,string("w"));
                            mp.process(string("stackdebugplot"));
			    }
#endif
                        }
                        ++icol;
                    }
                }
            }
            // normalize the stack.
            // not trivial for a variety of reasons
	    // This uses a threshold to avoid divide by zero but 
	    // no other complexity.
	    for(i=0;i<nsout;++i)
            {
                if(stack_weight[i]>WEIGHT_MINIMUM)
                {
                    for(j=0;j<3;++j) stack(j,i)/=stack_weight[i];
                }
                else
                {
                    for(j=0;j<3;++j) stack(j,i)=0.0;
                }
            }

//DEBUG
/*
dcopy(nsout*3,stack.get_address(0,0),1,DEBUGBUFFER,1);
twork.clear();
for(int kkk=0;kkk<(3*nsout);++kkk) twork.push_back(DEBUGBUFFER[kkk]);
idbg=max_element(twork.begin(),twork.end());
cout << "Max of stack="<<*idbg<<endl;
*/
            // Create the output stack as a 3c trace object and copy
            // metadata from the input into the output object.
            stackout = new ThreeComponentSeismogram(nsout);
            stackout->dt=dt;
            stackout->t0=tstart;
            stackout->live=true;
            stackout->u=stack;
            copy_selected_metadata(dynamic_cast<Metadata&>(indata),
                dynamic_cast<Metadata&>(*stackout),mdlcopy);
            stackout->put("ix1",ix1);
            stackout->put("ix2",ix2);
            stackout->put("ux",ux);
            stackout->put("uy",uy);
            stackout->put("gridid",gridid);
            stackout->put("dux",dux);
            stackout->put("duy",duy);
            stackout->put("ux0",ux0);
            stackout->put("uy0",uy0);
            // may want to output a static here, but it is probably better to
            // just keep a good estimate of elevation and deal with this in the
            // migration algorithm.
            stackout->put("elev",avg_elev);
            ApplyTopMute(*stackout,smuteused);
//DEBUG
/*
dcopy(nsout*3,stackout->u.get_address(0,0),1,DEBUGBUFFER,1);
twork.clear();
for(int kkk=0;kkk<(3*nsout);++kkk) twork.push_back(DEBUGBUFFER[kkk]);
idbg=max_element(twork.begin(),twork.end());
cout << "Max of stackout="<<*idbg<<endl;
*/
#ifdef MATLABDEBUG
             mp.load(*stackout,string("d"));
             mp.load(stack_weight,string("sw"));
             mp.process(string("pwsplot"));
#endif
            // new March 2007: compute stack coherence
            Coharray coh=compute_stack_coherence(gather,gathwgt,*stackout,
                dtcoh,cohwinlen,smuteused);
            stacklist.push_back(*stackout);
            delete stackout;
            coharraylist.push_back(coh);
        }
    }
    list<ThreeComponentSeismogram>::iterator soutptr;
    list<Coharray>::iterator cohptr;
    /* Loop over the results of above.  Note we assume here that
    coharraylist and stacklist are the same length.*/
    for(soutptr=stacklist.begin(),cohptr=coharraylist.begin();
        soutptr!=stacklist.end();++soutptr,++cohptr)
    {
        try
        {
	    dfh.save(*soutptr);
	    save_coh(*cohptr,*soutptr,coh3cfh,cohfh);
        }
        catch(SeisppError& err)
        {
            err.log_error();
            cerr << "Write failure abort:  cannot continue"<<endl;
            exit(-1);
        }
    }
    return(stack_count);
}
