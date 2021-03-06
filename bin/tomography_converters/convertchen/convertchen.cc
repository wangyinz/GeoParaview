#include <iostream>
#include <fstream>
#include "seispp.h"
#include "gclgrid.h"
using namespace std;
using namespace SEISPP;
/* this is a special program to convert Chen's surface wave model for
   OIINK region to GCLgrid format
   */
/*
2016.2.5, Xiaotao Yang: add option to output velocity model in vector gclgrid format, which saves 4 components for each grid.
*/ 

void usage()
{
    cout << "convertchen outfile [-rho|-vp|-vs|-vector -dir outdir] < infile"<<endl
        << "  Reads model txt file from stdin."<<endl
        << "  Flags select alternate properties (default is vs)"<<endl
        << "  -vector: will save all three attributes to vector gclfield. "<<endl
        << "  -dir will write to outdir (warning must already exist)"<<endl
        << "     default is current directory"<<endl;
    exit(-1);
}
bool SEISPP::SEISPP_verbose(true);
int main(int argc, char **argv)
{
  try {
    ios::sync_with_stdio();
    if(argc<2) usage();
    string outfile(argv[1]);
    string outdir(".");
    cout << "Will write results to base file name "<<outfile
        <<" in GCLgrid file format"<<endl;
    /* We have some argument parsing complexity here to select
       the correct column of data.  We use an integer index to
       define this for the column number */
    int data_column_to_read(3);
    string savecomponent("S");
    bool isvector(false);
    int i,j,k,kk;
    for(i=2;i<argc;++i)
    {
        string sarg(argv[i]);
        if(sarg=="-rho")
        {    data_column_to_read=1;
            savecomponent="Density";}
        else if(sarg=="-vp")
        {    data_column_to_read=2;
            savecomponent="P";}
        else if(sarg=="-vs")
        {    data_column_to_read=3;
            savecomponent="S";}
        else if(sarg=="-vector")
        {    isvector=true;
            savecomponent="ALL";}
        else if(sarg=="-dir")
        {
            ++i;
            if(i>=argc)usage();
            outdir=string(argv[i]);
        }
        else
            usage();
    }
    /* Will make origin first point in ascii file */
    double lat0,lon0;
    double dlat,dlon,latmax,lonmax;
    /* Using C++ i/o because for some odd reason this set of things
       are in two lines. */
    cin >> lat0;  cin>>latmax;  cin>>dlat;
    cin >> lon0;  cin>>lonmax;  cin>>dlon;
    /* We have to compute grid size with this format*/
    int n1,n2;
    n1=SEISPP::nint((lonmax-lon0)/dlon)+1;
    n2=SEISPP::nint((latmax-lat0)/dlat)+1;
    /* gclgrid requires these in radians */
    lat0=rad(lat0);
    lon0=rad(lon0);
    double r0=r0_ellipse(lat0);
    /* Read the first two vertical scan header lines.  We will
       need to require all subsequent blocks have the same 
       number of layers */
    int n3;
    /* These are actual lat lon read for each model scan.  We do 
       not depend on the uniform grid globals.  This also simplifies
       read loop below */
    double lat,lon;   
    /* This one needs to be frozen as a constant.  Not critical
       anyway so this is an unimportant issue. */
    const double dx3nom(10.0);
    double dx1nom,dx2nom;
    dx1nom=dlon*111.0;  // not bothering with cos correction
    dx2nom=dlat*111.0;
    const int i0(0),j0(0);
    /* Need this a a pointer so we can create it below.  A bit awkward
       but a solution for this file format.  Problem is we have to 
       read the next two lines of the file to get n3, which is required
       to define the size of the grid. */
    if(!isvector)
    {
		cout<<"Converting to scalar gclfield3d ..."<<endl;
		cout<<"Will save attribute [ "<<savecomponent<<" ]"<<endl;
		GCLscalarfield3d *f;
		int nscans=n1*n2;
		for(i=0;i<nscans;++i)
		{
			double d[4];  // holds data read from each line
			vector<double> z,fval;
			if(fscanf(stdin,"%lf%lf",&lon,&lat)!=2)
			{
				cerr << "EOF encountered before read completed"<<endl
					<< "Hit EOF while trying to read first line of block "
					<< i<<endl;
				exit(-1);
			}
			cin>>n3;
			/* On first scan we create the grid.  As noted we need the
			   n3 we just read to know the size */
			if(i==0)
			{
				const string gridname("OIINK_SW");
				GCLgrid3d grid(n1,n2,n3,gridname,lat0,lon0,r0,0.0,
						dx1nom,dx2nom,dx3nom,i0,j0);
				f=new GCLscalarfield3d(grid);
			}
			else
			{
				/* This resets each of these stl vectors after
				   each scan.  Otherwise they will grow bigger
				   in each pass. */
				z.clear();
				fval.clear();
			}
			/* A sanity check */
			if(n3!=f->n3)
			{
				cerr << "Mismatch in number of model points in vertical direction"<<endl
					<< "This program assumes this is constant."<<endl
					<< "Check data - code may need to be fixed"<<endl;
				exit(-1);
			}
			/* Typical mismatch between thickness and layer tops.  
			   Assume top is at zero depth */
			z.push_back(0.0);
			double current_depth;
			/* this reads each block into the work vectors.*/
			for(j=0,current_depth=0.0;j<n3;++j)
			{
				for(k=0;k<4;++k) cin >> d[k];
				current_depth+=d[0];
				z.push_back(current_depth); // note z is one ahead of fval
				fval.push_back(d[data_column_to_read]);
			}
			/* Note that z will have an extra row we component we 
			   drop.   This will truncate the bottom layer but 
			   the presumption is this is never resolved anyway so
			   adding it would be baggage.  As a result we will
			   just these data now and put them in the GCLscalarfield 
			   object.  First we need to compute the index values*/
			int ii,jj;   // index positions for x1 and x2 grid directions
			ii=i%n1;
			jj=i/n1;
			lat=rad(lat);
			lon=rad(lon);
			double rsurface=r0_ellipse(lat);
			double r;
			/* Note need for indices running in opposite direction */
			for(k=0,kk=n3-1;k<n3;++k,--kk)
			{
				//cout << ii <<", "<<jj<<", "<<kk<<endl;
				Cartesian_point cp;
				r=rsurface-z[k];
				cp=f->gtoc(lat,lon,r);
				f->x1[ii][jj][kk]=cp.x1;
				f->x2[ii][jj][kk]=cp.x2;
				f->x3[ii][jj][kk]=cp.x3;
				f->val[ii][jj][kk]=fval[k];
			}
		}
		f->save(outfile,outdir);
    }
    else
/* Now fill in the values for the vector field.   In all cases we 
       write the field values in vector slots with 0-vp, 1-vs, 2-rho,
       and 3-thickness.  Eventually these could be space variable. */
    {
    	cout<<"Converting to vector gclfield3d ..."<<endl;
    	cout<<"Will save attribute [ "<<savecomponent<<" ]"<<endl;
    	GCLvectorfield3d *f;
		int nscans=n1*n2;
		for(i=0;i<nscans;++i)
		{
			double d[4];  // holds data read from each line
			vector<double> z,fval[4]; //h, for layer thickness
			//fval[0],[1],[2],[3]: vp, vs, rho, thickness, respectively.
			if(fscanf(stdin,"%lf%lf",&lon,&lat)!=2)
			{
				cerr << "EOF encountered before read completed"<<endl
					<< "Hit EOF while trying to read first line of block "
					<< i<<endl;
				exit(-1);
			}
			//cout<<"point: "<<lon<<", "<<lat<<endl;
			cin>>n3;
			/* On first scan we create the grid.  As noted we need the
			   n3 we just read to know the size */
			if(i==0)
			{
				const string gridname("OIINK_SW");
				GCLgrid3d grid(n1,n2,n3,gridname,lat0,lon0,r0,0.0,
						dx1nom,dx2nom,dx3nom,i0,j0);
				f=new GCLvectorfield3d(grid,4);
			}
			else
			{
				/* This resets each of these stl vectors after
				   each scan.  Otherwise they will grow bigger
				   in each pass. */
				z.clear();
				fval[0].clear();
				fval[1].clear();
				fval[2].clear();
				fval[3].clear();
			}
			/* A sanity check */
			if(n3!=f->n3)
			{
				cerr << "Mismatch in number of model points in vertical direction"<<endl
					<< "This program assumes this is constant."<<endl
					<< "Check data - code may need to be fixed"<<endl;
				exit(-1);
			}
			/* Typical mismatch between thickness and layer tops.  
			   Assume top is at zero depth */
			z.push_back(0.0);
			double current_depth;//, thickness;
			/* this reads each block into the work vectors.*/
			//cout<<"1 point: "<<lon<<", "<<lat<<endl;
			for(j=0,current_depth=0.0;j<n3;++j)
			{
				for(k=0;k<4;++k) cin >> d[k];
				current_depth+=d[0];
				z.push_back(current_depth);
				//cout<<"depth "<<current_depth<<endl;
				//thickness=d[0];
				//t.push_back(thickness); //
				fval[0].push_back(d[2]); //vp
				//cout<<"vp= "<<d[2]<<endl;
				fval[1].push_back(d[3]);//vs
				//cout<<"vs= "<<d[3]<<endl;
				fval[2].push_back(d[1]);//rho
				//cout<<"rho= "<<d[1]<<endl;
				fval[3].push_back(d[0]);
				//cout<<"thickness= "<<d[0]<<endl;
			}
			//cout<<"2 point: "<<lon<<", "<<lat<<endl;
			/* Note that z will have an extra row we component we 
			   drop.   This will truncate the bottom layer but 
			   the presumption is this is never resolved anyway so
			   adding it would be baggage.  As a result we will
			   just these data now and put them in the GCLscalarfield 
			   object.  First we need to compute the index values*/
			int ii,jj;   // index positions for x1 and x2 grid directions
			ii=i%n1;
			jj=i/n1;
			lat=rad(lat);
			lon=rad(lon);
			double rsurface=r0_ellipse(lat);
			double r;
			/* Note need for indices running in opposite direction */
			for(k=0,kk=n3-1;k<n3;++k,--kk)
			{
				//cout << ii <<", "<<jj<<", "<<kk<<endl;
				Cartesian_point cp;
				r=rsurface-z[k];
				cp=f->gtoc(lat,lon,r);
				f->x1[ii][jj][kk]=cp.x1;
				f->x2[ii][jj][kk]=cp.x2;
				f->x3[ii][jj][kk]=cp.x3;
				f->val[ii][jj][kk][0]=fval[0][k];
				f->val[ii][jj][kk][1]=fval[1][k];
				f->val[ii][jj][kk][2]=fval[2][k];
				f->val[ii][jj][kk][3]=fval[3][k];
			}
		}
		f->save(outfile,outdir);
    }
  } catch (int ierr)
  {
        cerr << "dbsave threw error code "<< ierr<<endl
            << "Probably no output"<<endl;
  }
  catch (std::exception& err)
  {
        cerr << "Standard library exception was thrown.  Message follows"
            <<endl;
        cerr << err.what()<<endl;
  }
}
